import { stat } from "node:fs/promises";

const abortError = (message = "Wait cancelled by the MCP client.") => {
  const error = new Error(message);
  error.name = "AbortError";
  return error;
};

export const abortableSleep = (ms, signal) => new Promise((resolve, reject) => {
  if (signal?.aborted) {
    reject(abortError());
    return;
  }
  let settled = false;
  const cleanup = () => signal?.removeEventListener("abort", onAbort);
  const timer = setTimeout(() => {
    if (settled) return;
    settled = true;
    cleanup();
    resolve();
  }, Math.max(0, ms));
  timer.unref?.();
  const onAbort = () => {
    if (settled) return;
    settled = true;
    clearTimeout(timer);
    cleanup();
    reject(abortError());
  };
  signal?.addEventListener("abort", onAbort, { once: true });
});

const pathState = async (filePath) => {
  try {
    const info = await stat(filePath);
    return {
      exists: true,
      isFile: info.isFile(),
      isDirectory: info.isDirectory(),
      size: info.size,
      mtimeMs: info.mtimeMs,
      mtimeUtc: new Date(info.mtimeMs).toISOString(),
    };
  } catch (error) {
    if (error?.code === "ENOENT") return { exists: false };
    if (["EPERM", "EBUSY", "ENOTDIR"].includes(error?.code)) {
      return { exists: null, transientError: error.code };
    }
    throw error;
  }
};

export const waitForPathCondition = async ({
  path,
  shouldExist = true,
  minBytes = 0,
  timeoutMs = 60000,
  pollMs = 250,
  stableMs = 0,
  signal,
} = {}) => {
  const startedAt = Date.now();
  const deadline = startedAt + Math.max(1, timeoutMs);
  let stableSince = null;
  let last = await pathState(path);

  while (true) {
    const conditionMet = shouldExist
      ? last.exists === true && Number(last.size ?? 0) >= Math.max(0, minBytes)
      : last.exists === false;
    if (conditionMet) {
      if (stableMs <= 0) {
        return { ...last, conditionMet: true, waitedMs: Date.now() - startedAt, timedOut: false };
      }
      stableSince ??= Date.now();
      if (Date.now() - stableSince >= stableMs) {
        return { ...last, conditionMet: true, stableMs, waitedMs: Date.now() - startedAt, timedOut: false };
      }
    } else {
      stableSince = null;
    }

    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      return { ...last, conditionMet: false, waitedMs: Date.now() - startedAt, timedOut: true };
    }
    await abortableSleep(Math.min(Math.max(50, pollMs), remaining), signal);
    last = await pathState(path);
  }
};

export const waitUtilsInternals = { pathState, abortError };
