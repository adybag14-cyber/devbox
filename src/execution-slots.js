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

const slotPath = (index) => path.join(slotRoot, `slot-${String(index).padStart(2, "0")}.json`);

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

export const acquireExecutionSlot = async ({
  kind = "interactive",
  maxConcurrent = 6,
  reservedInteractive = 1,
  queueTimeoutMs = 15000,
  signal,
  label = "execution",
} = {}) => {
  const total = Math.max(1, Number(maxConcurrent) || 1);
  const reserved = Math.max(0, Math.min(total - 1, Number(reservedInteractive) || 0));
  const usableSlots = kind === "background" ? Math.max(1, total - reserved) : total;
  const timeoutMs = Math.max(1, Number(queueTimeoutMs) || 1);
  const token = randomUUID();
  const queuedAt = Date.now();
  await mkdir(slotRoot, { recursive: true });
  metrics.queued += 1;

  try {
    while (true) {
      if (signal?.aborted) {
        metrics.cancelled += 1;
        throw abortError();
      }

      for (let index = 0; index < usableSlots; index += 1) {
        const filePath = slotPath(index);
        let handle = null;
        try {
          handle = await open(filePath, "wx");
          const queueWaitMs = Date.now() - queuedAt;
          await handle.writeFile(`${JSON.stringify({
            token,
            pid: process.pid,
            kind,
            label,
            acquiredAtUtc: new Date().toISOString(),
          })}\n`, "utf8");
          await handle.close();
          handle = null;
          metrics.queued -= 1;
          metrics.active += 1;
          metrics.acquired += 1;
          metrics.totalQueueWaitMs += queueWaitMs;
          metrics.maxQueueWaitMs = Math.max(metrics.maxQueueWaitMs, queueWaitMs);
          let released = false;
          return {
            slot: index,
            kind,
            queueWaitMs,
            async release() {
              if (released) return;
              released = true;
              metrics.active = Math.max(0, metrics.active - 1);
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
          if (await removeStaleSlot(filePath)) index -= 1;
        }
      }

      const elapsed = Date.now() - queuedAt;
      if (elapsed >= timeoutMs) {
        metrics.timedOut += 1;
        throw new ExecutionQueueTimeoutError(
          `Execution queue remained saturated for ${elapsed} ms. Retry shortly or use devbox_exec_start for long work.`,
          { kind, label, queue_wait_ms: elapsed, max_concurrent: total, reserved_interactive: reserved },
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

export const getExecutionSlotSnapshot = async ({ maxConcurrent = 6, reservedInteractive = 1 } = {}) => {
  const total = Math.max(1, Number(maxConcurrent) || 1);
  const reserved = Math.max(0, Math.min(total - 1, Number(reservedInteractive) || 0));
  await mkdir(slotRoot, { recursive: true });
  const entries = [];
  for (const name of await readdir(slotRoot).catch(() => [])) {
    if (!/^slot-\d+\.json$/u.test(name)) continue;
    const filePath = path.join(slotRoot, name);
    try {
      const value = JSON.parse(await readFile(filePath, "utf8"));
      if (!processAlive(Number(value.pid))) {
        await rm(filePath, { force: true });
        continue;
      }
      entries.push({ slot: Number(name.match(/\d+/u)?.[0]), ...value });
    } catch {
    }
  }
  return {
    max_concurrent: total,
    reserved_interactive: reserved,
    background_capacity: Math.max(1, total - reserved),
    occupied: entries.length,
    occupied_slots: entries,
    local_process: {
      queued: metrics.queued,
      active: metrics.active,
      acquired: metrics.acquired,
      timed_out: metrics.timedOut,
      cancelled: metrics.cancelled,
      average_queue_wait_ms: metrics.acquired > 0 ? Math.round(metrics.totalQueueWaitMs / metrics.acquired) : 0,
      max_queue_wait_ms: metrics.maxQueueWaitMs,
    },
  };
};
