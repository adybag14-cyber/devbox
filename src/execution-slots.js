import { randomUUID } from "node:crypto";
import { mkdir, open, readFile, readdir, rm, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const slotRoot = process.env.MCP_EXEC_SLOT_ROOT?.trim()
  ? path.resolve(process.env.MCP_EXEC_SLOT_ROOT.trim())
  : path.join(projectRoot, "run", "execution-slots");
const POLL_MS = 50;
const CORRUPT_SLOT_STALE_MS = 5 * 60 * 1000;

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
  return ["watch", "light", "heavy"].includes(normalized) ? normalized : "light";
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

const removeStaleSlot = async (filePath) => {
  let owner = null;
  try {
    owner = JSON.parse(await readFile(filePath, "utf8"));
  } catch {
  }

  if (Number.isInteger(owner?.pid) && processAlive(owner.pid)) {
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
  for (const { filePath, token } of owned) {
    try {
      const current = JSON.parse(await readFile(filePath, "utf8"));
      if (current?.token === token) await rm(filePath, { force: true });
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
    }
  }
};

export const acquireExecutionSlot = async ({
  kind = "interactive",
  resourceClass = "light",
  weight = 1,
  maxConcurrent = 6,
  reservedInteractive = 1,
  watchMaxConcurrent = 4,
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
  const usableSlots = kind === "background" && pool === "execution" ? Math.max(1, total - reserved) : total;
  const requestedWeight = pool === "watch"
    ? 1
    : Math.max(1, Math.min(usableSlots, Number(weight) || 1));
  const timeoutMs = Math.max(1, Number(queueTimeoutMs) || 1);
  const queuedAt = Date.now();
  await mkdir(slotRoot, { recursive: true });
  metrics.queued += 1;

  try {
    while (true) {
      if (signal?.aborted) {
        metrics.cancelled += 1;
        throw abortError();
      }

      const owned = [];
      for (let index = 0; index < usableSlots && owned.length < requestedWeight; index += 1) {
        const filePath = slotPath(pool, index);
        const token = randomUUID();
        let handle = null;
        try {
          handle = await open(filePath, "wx");
          await handle.writeFile(`${JSON.stringify({
            token,
            pid: process.pid,
            kind,
            pool,
            resourceClass: normalizedClass,
            weight: requestedWeight,
            label,
            acquiredAtUtc: new Date().toISOString(),
          })}\n`, "utf8");
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

      if (owned.length === requestedWeight) {
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
      await sleep(Math.min(POLL_MS, timeoutMs - elapsed));
    }
  } catch (error) {
    if (metrics.queued > 0) metrics.queued -= 1;
    throw error;
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
      if (!processAlive(Number(value.pid))) {
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
} = {}) => {
  const total = Math.max(1, Number(maxConcurrent) || 1);
  const reserved = Math.max(0, Math.min(total - 1, Number(reservedInteractive) || 0));
  const watchTotal = Math.max(1, Number(watchMaxConcurrent) || 1);
  await mkdir(slotRoot, { recursive: true });
  const [entries, watchEntries] = await Promise.all([readPoolEntries("execution"), readPoolEntries("watch")]);
  const byClass = Object.fromEntries([...metrics.byClass.entries()].map(([name, value]) => [name, {
    active: value.active,
    acquired: value.acquired,
    average_queue_wait_ms: value.acquired > 0 ? Math.round(value.totalQueueWaitMs / value.acquired) : 0,
    max_queue_wait_ms: value.maxQueueWaitMs,
  }]));
  return {
    max_concurrent: total,
    reserved_interactive: reserved,
    background_capacity: Math.max(1, total - reserved),
    watch_capacity: watchTotal,
    occupied: entries.length,
    occupied_slots: entries,
    watch_occupied: watchEntries.length,
    watch_slots: watchEntries,
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

export const executionSlotInternals = { normalizeResourceClass, poolFor, processAlive, slotPath };
