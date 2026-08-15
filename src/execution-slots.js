import { randomUUID } from "node:crypto";
import { mkdir, open, readFile, readdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { currentProcessInstance, processMatchesInstance } from "./process-identity.js";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const slotRoot = process.env.MCP_EXEC_SLOT_ROOT?.trim()
  ? path.resolve(process.env.MCP_EXEC_SLOT_ROOT.trim())
  : path.join(projectRoot, "run", "execution-slots");
const diskPressureStatePath = path.join(slotRoot, ".disk-pressure.json");
const POLL_MS = 50;
const MAX_POLL_MS = 500;
const pollInterval = (elapsedMs) => elapsedMs < 1000 ? POLL_MS : elapsedMs < 5000 ? 100 : elapsedMs < 30000 ? 250 : MAX_POLL_MS;
const CORRUPT_SLOT_STALE_MS = 5 * 60 * 1000;
const CORRUPT_QUEUE_TICKET_STALE_MS = 5000;
const DISK_PRESSURE_STATE_STALE_MS = 180_000;
const DISK_PRESSURE_CACHE_MS = 1_000;
let diskPressureCache = { sampledAtMs: 0, constrained: false };

const metrics = {
  queued: 0,
  active: 0,
  acquired: 0,
  timedOut: 0,
  cancelled: 0,
  totalQueueWaitMs: 0,
  maxQueueWaitMs: 0,
  byClass: new Map(),
};

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const processAlive = (pid) => {
  if (!Number.isInteger(pid) || pid < 1) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return process.platform === "win32" && error?.code === "EPERM";
  }
};

const diskPressureConstrained = async () => {
  const now = Date.now();
  if (now - diskPressureCache.sampledAtMs < DISK_PRESSURE_CACHE_MS) return diskPressureCache.constrained;
  let constrained = false;
  try {
    const info = await stat(diskPressureStatePath);
    if (now - info.mtimeMs <= DISK_PRESSURE_STATE_STALE_MS) {
      const state = JSON.parse(await readFile(diskPressureStatePath, "utf8"));
      constrained = ["warning", "critical"].includes(String(state?.diskPressure ?? ""));
    }
  } catch {
    constrained = false;
  }
  diskPressureCache = { sampledAtMs: now, constrained };
  return constrained;
};

const abortError = () => {
  const error = new Error("Execution queue wait cancelled by the MCP client.");
  error.name = "AbortError";
  return error;
};

export class ExecutionQueueTimeoutError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "ExecutionQueueTimeoutError";
    this.data = details;
  }
}

const normalizeResourceClass = (value) => {
  const normalized = String(value ?? "light").trim().toLowerCase();
  return ["watch", "light", "heavy", "io-heavy"].includes(normalized) ? normalized : "light";
};

const classMetrics = (resourceClass) => {
  const key = normalizeResourceClass(resourceClass);
  let value = metrics.byClass.get(key);
  if (!value) {
    value = { acquired: 0, active: 0, totalQueueWaitMs: 0, maxQueueWaitMs: 0 };
    metrics.byClass.set(key, value);
  }
  return value;
};

const poolFor = ({ kind, resourceClass }) =>
  kind === "background" && normalizeResourceClass(resourceClass) === "watch" ? "watch" : "execution";

const slotPath = (pool, index) => path.join(
  slotRoot,
  `${pool === "watch" ? "watch-slot" : "slot"}-${String(index).padStart(2, "0")}.json`,
);

const queueRoot = path.join(slotRoot, "queue");
const queueClassFor = ({ kind, pool, resourceClass, pressureConstrained = false }) => {
  if (pool === "watch") return "watch";
  if (kind !== "interactive") return "execution-background";
  if (!pressureConstrained) return "execution-interactive";
  return ["heavy", "io-heavy"].includes(normalizeResourceClass(resourceClass))
    ? "execution-interactive-weighted"
    : "execution-interactive-light";
};
const queueHeadPath = (queueClass) => path.join(queueRoot, `.${queueClass}-head.json`);
const queueHeadLockPath = (queueClass) => path.join(queueRoot, `.${queueClass}-head.lock`);
const queueSequencePath = (queueClass) => path.join(queueRoot, `.${queueClass}-sequence.txt`);
const queueTicketPrefix = (queueClass) => `${queueClass}-`;
const queueTimestampNs = () => BigInt(Date.now()) * 1_000_000n;

const atomicReplaceText = async (filePath, text) => {
  const temporary = `${filePath}.${process.pid}.${randomUUID()}.tmp`;
  await writeFile(temporary, text, "utf8");
  try {
    await rename(temporary, filePath);
  } catch (error) {
    await rm(temporary, { force: true }).catch(() => {});
    throw error;
  }
};

const acquireQueueHeadLock = async (queueClass, { signal, deadlineMs }) => {
  await mkdir(queueRoot, { recursive: true });
  const filePath = queueHeadLockPath(queueClass);
  const boundedDeadline = Math.min(deadlineMs, Date.now() + 5000);
  while (Date.now() < boundedDeadline) {
    if (signal?.aborted) throw abortError();
    const token = randomUUID();
    let handle = null;
    try {
      handle = await open(filePath, "wx");
      await handle.writeFile(`${JSON.stringify({ token, pid: process.pid, processInstance: await currentProcessInstance(), class: queueClass, acquiredAtUtc: new Date().toISOString() })}\n`, "utf8");
      await handle.close();
      handle = null;
      let released = false;
      return {
        async release() {
          if (released) return;
          released = true;
          try {
            const current = JSON.parse(await readFile(filePath, "utf8"));
            if (current?.token === token) await rm(filePath, { force: true });
          } catch (error) {
            if (error?.code !== "ENOENT") throw error;
          }
        },
      };
    } catch (error) {
      try { await handle?.close(); } catch {}
      if (error?.code !== "EEXIST") throw error;
      if (await removeStaleSlot(filePath)) continue;
      await sleep(10);
    }
  }
  throw new Error(`timed out acquiring queue-head lock for ${queueClass}`);
};

const nextQueueSequence = async (queueClass, { signal, deadlineMs }) => {
  const lock = await acquireQueueHeadLock(queueClass, { signal, deadlineMs });
  try {
    const filePath = queueSequencePath(queueClass);
    const current = await readFile(filePath, "utf8")
      .then((value) => BigInt(value.trim()))
      .catch(() => null);
    const seed = queueTimestampNs();
    const next = (current === null || current < seed ? seed : current) + 1n;
    await atomicReplaceText(filePath, `${next}\n`);
    return next;
  } finally {
    await lock.release();
  }
};

const refreshQueueHead = async (queueClass, { signal, deadlineMs }) => {
  const lock = await acquireQueueHeadLock(queueClass, { signal, deadlineMs });
  try {
    const prefix = queueTicketPrefix(queueClass);
    const names = (await readdir(queueRoot).catch(() => []))
      .filter((name) => name.startsWith(prefix) && name.endsWith(".json"))
      .sort();
    const head = names[0] ?? null;
    const filePath = queueHeadPath(queueClass);
    if (head === null) {
      await rm(filePath, { force: true }).catch(() => {});
    } else {
      await atomicReplaceText(filePath, `${JSON.stringify({ name: head })}\n`);
    }
    return head;
  } finally {
    await lock.release();
  }
};

const ticketExpired = (name, owner) => {
  const timeoutMs = Math.max(1, Number(owner?.queueTimeoutMs) || CORRUPT_SLOT_STALE_MS);
  let queuedAtMs = Number(owner?.queuedAtUnixMs);
  if (!Number.isFinite(queuedAtMs)) {
    const queueClass = String(owner?.class ?? "");
    const raw = name.startsWith(`${queueClass}-`) ? name.slice(queueClass.length + 1).split("-")[0] : "";
    try { queuedAtMs = Number(BigInt(raw) / 1_000_000n); } catch { queuedAtMs = NaN; }
  }
  return Number.isFinite(queuedAtMs) && Date.now() - queuedAtMs > timeoutMs + 1000;
};

const ticketShouldReap = async (name, owner) => {
  const alive = await processMatchesInstance(Number(owner?.pid), owner?.processInstance);
  if (!alive) return true;
  const hasIdentity = owner?.processInstance !== null && owner?.processInstance !== undefined && owner?.processInstance !== "";
  return !hasIdentity && ticketExpired(name, owner);
};

const createQueueTicket = async ({ kind, pool, resourceClass, weight, label, timeoutMs, signal, deadlineMs, pressureConstrained }) => {
  await mkdir(queueRoot, { recursive: true });
  const queueClass = queueClassFor({ kind, pool, resourceClass, pressureConstrained });
  const sequence = await nextQueueSequence(queueClass, { signal, deadlineMs });
  const token = randomUUID();
  const name = `${queueClass}-${sequence.toString().padStart(32, "0")}-${token}.json`;
  const filePath = path.join(queueRoot, name);
  let handle = null;
  try {
    handle = await open(filePath, "wx");
    await handle.writeFile(`${JSON.stringify({
      token,
      pid: process.pid,
      processInstance: await currentProcessInstance(),
      class: queueClass,
      kind,
      resourceClass,
      weight,
      label,
      sequence: sequence.toString(),
      queuedAtUnixMs: Date.now(),
      queuedAtUtc: new Date().toISOString(),
      queueTimeoutMs: timeoutMs,
    })}\n`, "utf8");
    await handle.close();
    handle = null;
    await refreshQueueHead(queueClass, { signal, deadlineMs });
  } catch (error) {
    try { await handle?.close(); } catch {}
    await rm(filePath, { force: true }).catch(() => {});
    throw error;
  }
  let released = false;
  return {
    queueClass,
    name,
    filePath,
    async release() {
      if (released) return;
      released = true;
      await rm(filePath, { force: true }).catch((error) => {
        if (error?.code !== "ENOENT") throw error;
      });
      await refreshQueueHead(queueClass, { signal: null, deadlineMs: Date.now() + 1000 });
    },
  };
};

const queueTicketIsHead = async (ticket, { signal, deadlineMs }) => {
  const prefix = queueTicketPrefix(ticket.queueClass);
  for (;;) {
    let head = await readFile(queueHeadPath(ticket.queueClass), "utf8")
      .then((value) => JSON.parse(value)?.name ?? null)
      .catch(() => null);
    if (!head) {
      head = await refreshQueueHead(ticket.queueClass, { signal, deadlineMs });
      if (!head) continue;
    }
    if (head === ticket.name) return true;
    const headPath = path.join(queueRoot, head);
    let owner = await readFile(headPath, "utf8").then(JSON.parse).catch(() => null);
    if (!owner) {
      const fresh = await stat(headPath).then((info) => Date.now() - info.mtimeMs < CORRUPT_QUEUE_TICKET_STALE_MS).catch(() => false);
      if (fresh) return false;
      await rm(headPath, { force: true }).catch(() => {});
      await refreshQueueHead(ticket.queueClass, { signal, deadlineMs });
      continue;
    }
    if (await ticketShouldReap(head, owner)) {
      await rm(headPath, { force: true }).catch(() => {});
      await refreshQueueHead(ticket.queueClass, { signal, deadlineMs });
      continue;
    }
    return false;
  }
};

const readQueueSnapshot = async () => {
  const result = {};
  for (const name of await readdir(queueRoot).catch(() => [])) {
    if (!name.endsWith(".json")) continue;
    const filePath = path.join(queueRoot, name);
    const owner = await readFile(filePath, "utf8").then(JSON.parse).catch(() => null);
    const queueClass = String(owner?.class ?? "");
    const knownClass = queueClass === "watch"
      || queueClass.startsWith("execution-interactive")
      || queueClass.startsWith("execution-background");
    if (!owner || !knownClass || await ticketShouldReap(name, owner)) {
      if (owner && knownClass) await rm(filePath, { force: true }).catch(() => {});
      continue;
    }
    result[queueClass] = (result[queueClass] ?? 0) + 1;
  }
  return result;
};

export const probeExecutionSlotStoreWritable = async () => {
  await mkdir(slotRoot, { recursive: true });
  const probePath = path.join(slotRoot, `.mcp-ready-${process.pid}-${randomUUID()}.tmp`);
  let handle = null;
  try {
    handle = await open(probePath, "wx");
    await handle.writeFile("ready\n", "utf8");
    await handle.sync();
    await handle.close();
    handle = null;
    await rm(probePath, { force: true });
    return true;
  } finally {
    try { await handle?.close(); } catch {}
    await rm(probePath, { force: true }).catch(() => {});
  }
};

const removeStaleSlot = async (filePath) => {
  let owner = null;
  try {
    owner = JSON.parse(await readFile(filePath, "utf8"));
  } catch {
  }

  if (Number.isInteger(owner?.pid) && await processMatchesInstance(owner.pid, owner.processInstance)) {
    return false;
  }

  if (!owner) {
    try {
      const info = await stat(filePath);
      if (Date.now() - info.mtimeMs < CORRUPT_SLOT_STALE_MS) return false;
    } catch (error) {
      return error?.code === "ENOENT";
    }
  }

  await rm(filePath, { force: true });
  return true;
};

const releaseOwnedFiles = async (owned) => {
  const errors = [];
  for (const { filePath, token } of owned) {
    try {
      const current = JSON.parse(await readFile(filePath, "utf8"));
      if (current?.token === token) await rm(filePath, { force: true });
    } catch (error) {
      if (error?.code !== "ENOENT") errors.push(error);
    }
  }
  if (errors.length > 0) {
    throw new AggregateError(errors, `Failed to release ${errors.length} execution-slot file(s).`);
  }
};

const claimLockPath = (pool) => path.join(slotRoot, `${pool}-weighted-claim.json`);

const acquirePoolClaimLock = async (pool, { signal, deadlineMs }) => {
  const filePath = claimLockPath(pool);
  while (Date.now() < deadlineMs) {
    if (signal?.aborted) throw abortError();
    const token = randomUUID();
    let handle = null;
    try {
      handle = await open(filePath, "wx");
      await handle.writeFile(`${JSON.stringify({
        token,
        pid: process.pid,
        processInstance: await currentProcessInstance(),
        pool,
        acquiredAtUtc: new Date().toISOString(),
      })}
`, "utf8");
      await handle.close();
      handle = null;
      let released = false;
      return {
        async release() {
          if (released) return;
          released = true;
          let lastError = null;
          for (let attempt = 0; attempt < 3; attempt += 1) {
            try {
              const current = JSON.parse(await readFile(filePath, "utf8"));
              if (current?.token === token) await rm(filePath, { force: true });
              return;
            } catch (error) {
              if (error?.code === "ENOENT") return;
              lastError = error;
              if (attempt < 2) await sleep(10 * (attempt + 1));
            }
          }
          throw lastError;
        },
      };
    } catch (error) {
      try { await handle?.close(); } catch {}
      if (error?.code !== "EEXIST") throw error;
      if (await removeStaleSlot(filePath)) continue;
      const remaining = deadlineMs - Date.now();
      if (remaining <= 0) break;
      await sleep(Math.min(remaining, 15 + Math.floor(Math.random() * 20)));
    }
  }
  return null;
};

const agedBackgroundWaiterExists = async (thresholdMs) => {
  const boundedThreshold = Math.max(1, Number(thresholdMs) || 30_000);
  const now = Date.now();
  for (const name of await readdir(queueRoot).catch(() => [])) {
    if (!name.startsWith("execution-background-") || !name.endsWith(".json")) continue;
    const owner = await readFile(path.join(queueRoot, name), "utf8").then(JSON.parse).catch(() => null);
    if (!owner || await ticketShouldReap(name, owner)) continue;
    const queuedAt = Number(owner.queuedAtUnixMs);
    if (Number.isFinite(queuedAt) && now - queuedAt >= boundedThreshold) return true;
  }
  return false;
};

export const acquireExecutionSlot = async ({
  kind = "interactive",
  resourceClass = "light",
  weight = 1,
  maxConcurrent = 6,
  reservedInteractive = 1,
  watchMaxConcurrent = 4,
  heavyCapacity = 4,
  heavyWeight = 2,
  ioHeavyCapacity = 2,
  ioHeavyWeight = 2,
  backgroundPriorityAgeMs = 30_000,
  queueTimeoutMs = 15000,
  signal,
  label = "execution",
} = {}) => {
  const normalizedClass = normalizeResourceClass(resourceClass);
  const pool = poolFor({ kind, resourceClass: normalizedClass });
  const executionTotal = Math.max(1, Number(maxConcurrent) || 1);
  const watchTotal = Math.max(1, Number(watchMaxConcurrent) || 1);
  const total = pool === "watch" ? watchTotal : executionTotal;
  const reserved = pool === "execution"
    ? Math.max(0, Math.min(total - 1, Number(reservedInteractive) || 0))
    : 0;
  const baseUsableSlots = kind === "background" && pool === "execution" ? Math.max(1, total - reserved) : total;
  const normalizedHeavyCapacity = Math.max(1, Math.min(total, Number(heavyCapacity) || total));
  const normalizedIoHeavyCapacity = Math.max(1, Math.min(total, Number(ioHeavyCapacity) || total));
  const pressureConstrained = await diskPressureConstrained();
  const requested = Math.max(1, Number(weight) || 1);
  const classCapacity = normalizedClass === "heavy"
    ? (pressureConstrained ? Math.min(normalizedHeavyCapacity, requested) : normalizedHeavyCapacity)
    : normalizedClass === "io-heavy"
      ? (pressureConstrained ? Math.min(normalizedIoHeavyCapacity, requested) : normalizedIoHeavyCapacity)
      : total;
  const usableSlots = pool === "execution" && ["heavy", "io-heavy"].includes(normalizedClass)
    ? Math.min(baseUsableSlots, Math.max(Number(weight) || 1, classCapacity))
    : baseUsableSlots;
  const requestedWeight = pool === "watch"
    ? 1
    : Math.max(1, Math.min(usableSlots, Number(weight) || 1));
  const protectedLowSlots = pool === "execution" && pressureConstrained && normalizedClass === "light"
    ? Math.min(
        Math.max(0, usableSlots - 1),
        Math.max(1, Number(heavyWeight) || 1, Number(ioHeavyWeight) || 1),
      )
    : 0;
  const timeoutMs = Math.max(1, Number(queueTimeoutMs) || 1);
  const queuedAt = Date.now();
  await mkdir(slotRoot, { recursive: true });
  metrics.queued += 1;
  const deadlineMs = queuedAt + timeoutMs;
  let ticket = null;

  try {
    ticket = await createQueueTicket({
      kind,
      pool,
      resourceClass: normalizedClass,
      weight: requestedWeight,
      label,
      timeoutMs,
      signal,
      deadlineMs,
      pressureConstrained,
    });
    while (true) {
      if (signal?.aborted) {
        metrics.cancelled += 1;
        throw abortError();
      }

      if (!(await queueTicketIsHead(ticket, { signal, deadlineMs }))) {
        const elapsed = Date.now() - queuedAt;
        if (elapsed >= timeoutMs) {
          metrics.timedOut += 1;
          throw new ExecutionQueueTimeoutError(
            `Execution queue remained saturated for ${elapsed} ms. Retry shortly or use a detached job for long work.`,
            { kind, label, pool, resource_class: normalizedClass, weight: requestedWeight, queue_wait_ms: elapsed, max_concurrent: total, reserved_interactive: reserved },
          );
        }
        await sleep(Math.min(pollInterval(elapsed), timeoutMs - elapsed));
        continue;
      }
      if (kind === "interactive" && await agedBackgroundWaiterExists(backgroundPriorityAgeMs)) {
        const elapsed = Date.now() - queuedAt;
        if (elapsed >= timeoutMs) {
          metrics.timedOut += 1;
          throw new ExecutionQueueTimeoutError(
            `Execution queue remained saturated for ${elapsed} ms while yielding to an aged background waiter.`,
            { kind, label, pool, resource_class: normalizedClass, weight: requestedWeight, queue_wait_ms: elapsed, max_concurrent: total, reserved_interactive: reserved },
          );
        }
        await sleep(Math.min(pollInterval(elapsed), timeoutMs - elapsed));
        continue;
      }
      const claimLock = requestedWeight > 1
        ? await acquirePoolClaimLock(pool, { signal, deadlineMs })
        : null;
      if (requestedWeight > 1 && !claimLock) {
        const elapsed = Date.now() - queuedAt;
        metrics.timedOut += 1;
        throw new ExecutionQueueTimeoutError(
          `Execution queue remained saturated for ${elapsed} ms while reserving weighted capacity.`,
          {
            kind,
            label,
            pool,
            resource_class: normalizedClass,
            weight: requestedWeight,
            queue_wait_ms: elapsed,
            max_concurrent: total,
            reserved_interactive: reserved,
          },
        );
      }

      const owned = [];
      let claimReleaseError = null;
      try {
        for (let index = protectedLowSlots; index < usableSlots && owned.length < requestedWeight; index += 1) {
          const filePath = slotPath(pool, index);
          const token = randomUUID();
          let handle = null;
          try {
            handle = await open(filePath, "wx");
            await handle.writeFile(`${JSON.stringify({
              token,
              pid: process.pid,
              processInstance: await currentProcessInstance(),
              kind,
              pool,
              resourceClass: normalizedClass,
              weight: requestedWeight,
              label,
              acquiredAtUtc: new Date().toISOString(),
            })}
`, "utf8");
            await handle.close();
            handle = null;
            owned.push({ filePath, token, index });
          } catch (error) {
            try { await handle?.close(); } catch {}
            if (error?.code !== "EEXIST") {
              await releaseOwnedFiles(owned).catch(() => {});
              throw error;
            }
            if (await removeStaleSlot(filePath)) index -= 1;
          }
        }
      } finally {
        try {
          await claimLock?.release();
        } catch (error) {
          claimReleaseError = error;
        }
      }
      if (claimReleaseError) {
        const releaseErrors = [];
        try {
          await releaseOwnedFiles(owned);
        } catch (error) {
          releaseErrors.push(error);
        }
        if (releaseErrors.length > 0) {
          throw new AggregateError([claimReleaseError, ...releaseErrors], "Weighted execution claim cleanup failed.");
        }
        throw claimReleaseError;
      }

      if (owned.length === requestedWeight) {
        try {
          await ticket.release();
          ticket = null;
        } catch (error) {
          await releaseOwnedFiles(owned).catch(() => {});
          throw error;
        }
        const queueWaitMs = Date.now() - queuedAt;
        metrics.queued -= 1;
        metrics.active += 1;
        metrics.acquired += 1;
        metrics.totalQueueWaitMs += queueWaitMs;
        metrics.maxQueueWaitMs = Math.max(metrics.maxQueueWaitMs, queueWaitMs);
        const perClass = classMetrics(normalizedClass);
        perClass.active += 1;
        perClass.acquired += 1;
        perClass.totalQueueWaitMs += queueWaitMs;
        perClass.maxQueueWaitMs = Math.max(perClass.maxQueueWaitMs, queueWaitMs);
        let released = false;
        return {
          slot: owned[0]?.index ?? null,
          slots: owned.map((entry) => entry.index),
          kind,
          pool,
          resourceClass: normalizedClass,
          weight: requestedWeight,
          queueWaitMs,
          async release() {
            if (released) return;
            released = true;
            metrics.active = Math.max(0, metrics.active - 1);
            perClass.active = Math.max(0, perClass.active - 1);
            await releaseOwnedFiles(owned);
          },
        };
      }

      await releaseOwnedFiles(owned).catch(() => {});
      const elapsed = Date.now() - queuedAt;
      if (elapsed >= timeoutMs) {
        metrics.timedOut += 1;
        throw new ExecutionQueueTimeoutError(
          `Execution queue remained saturated for ${elapsed} ms. Retry shortly or use a detached job for long work.`,
          {
            kind,
            label,
            pool,
            resource_class: normalizedClass,
            weight: requestedWeight,
            queue_wait_ms: elapsed,
            max_concurrent: total,
            reserved_interactive: reserved,
          },
        );
      }
      await sleep(Math.min(pollInterval(elapsed), timeoutMs - elapsed));
    }
  } catch (error) {
    if (metrics.queued > 0) metrics.queued -= 1;
    throw error;
  } finally {
    await ticket?.release().catch(() => {});
  }
};

export const withExecutionSlot = async (options, callback) => {
  const lease = await acquireExecutionSlot(options);
  try {
    return await callback(lease);
  } finally {
    await lease.release();
  }
};

const readPoolEntries = async (pool) => {
  const prefix = pool === "watch" ? /^watch-slot-(\d+)\.json$/u : /^slot-(\d+)\.json$/u;
  const entries = [];
  for (const name of await readdir(slotRoot).catch(() => [])) {
    const match = name.match(prefix);
    if (!match) continue;
    const filePath = path.join(slotRoot, name);
    try {
      const value = JSON.parse(await readFile(filePath, "utf8"));
      if (!await processMatchesInstance(Number(value.pid), value.processInstance)) {
        await rm(filePath, { force: true });
        continue;
      }
      entries.push({ slot: Number(match[1]), ...value });
    } catch {
    }
  }
  return entries.sort((a, b) => a.slot - b.slot);
};

export const getExecutionSlotSnapshot = async ({
  maxConcurrent = 6,
  reservedInteractive = 1,
  watchMaxConcurrent = 4,
  heavyCapacity = 4,
  ioHeavyCapacity = 2,
  backgroundPriorityAgeMs = 30_000,
} = {}) => {
  const total = Math.max(1, Number(maxConcurrent) || 1);
  const reserved = Math.max(0, Math.min(total - 1, Number(reservedInteractive) || 0));
  const watchTotal = Math.max(1, Number(watchMaxConcurrent) || 1);
  await mkdir(slotRoot, { recursive: true });
  const [entries, watchEntries, queued] = await Promise.all([
    readPoolEntries("execution"),
    readPoolEntries("watch"),
    readQueueSnapshot(),
  ]);
  const byClass = Object.fromEntries([...metrics.byClass.entries()].map(([name, value]) => [name, {
    active: value.active,
    acquired: value.acquired,
    average_queue_wait_ms: value.acquired > 0 ? Math.round(value.totalQueueWaitMs / value.acquired) : 0,
    max_queue_wait_ms: value.maxQueueWaitMs,
  }]));
  return {
    max_concurrent: total,
    reserved_interactive: reserved,
    heavy_capacity: Math.max(1, Math.min(total, Number(heavyCapacity) || total)),
    io_heavy_capacity: Math.max(1, Math.min(total, Number(ioHeavyCapacity) || total)),
    background_priority_age_ms: Math.max(1, Number(backgroundPriorityAgeMs) || 30_000),
    background_capacity: Math.max(1, total - reserved),
    watch_capacity: watchTotal,
    occupied: entries.length,
    occupied_slots: entries,
    watch_occupied: watchEntries.length,
    watch_slots: watchEntries,
    global_queued: Object.values(queued).reduce((sum, value) => sum + value, 0),
    global_queued_by_class: queued,
    local_process: {
      queued: metrics.queued,
      active: metrics.active,
      acquired: metrics.acquired,
      timed_out: metrics.timedOut,
      cancelled: metrics.cancelled,
      average_queue_wait_ms: metrics.acquired > 0 ? Math.round(metrics.totalQueueWaitMs / metrics.acquired) : 0,
      max_queue_wait_ms: metrics.maxQueueWaitMs,
      by_resource_class: byClass,
    },
  };
};

export const executionSlotInternals = { normalizeResourceClass, poolFor, processAlive, slotPath, queueClassFor, ticketExpired };
