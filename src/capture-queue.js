const waiters = [];
let active = false;

const timeoutError = (waitedMs) => {
  const error = new Error(`Screen capture worker remained busy for ${waitedMs} ms.`);
  error.name = "CaptureQueueTimeoutError";
  error.data = { queue_wait_ms: waitedMs };
  return error;
};

const abortError = () => {
  const error = new Error("Screen capture queue wait cancelled by the MCP client.");
  error.name = "AbortError";
  return error;
};

const dispatch = () => {
  if (active) return;
  while (waiters.length > 0) {
    const waiter = waiters.shift();
    if (waiter.signal?.aborted) {
      waiter.reject(abortError());
      continue;
    }
    active = true;
    waiter.resolve({
      queueWaitMs: Date.now() - waiter.startedAt,
      release() {
        if (!active) return;
        active = false;
        queueMicrotask(dispatch);
      },
    });
    return;
  }
};

export const acquireCaptureWorker = ({ timeoutMs = 5000, signal } = {}) => new Promise((resolve, reject) => {
  if (signal?.aborted) {
    reject(abortError());
    return;
  }
  const waiter = { resolve, reject, signal, startedAt: Date.now() };
  let timer = null;
  const onAbort = () => {
    const index = waiters.indexOf(waiter);
    if (index >= 0) waiters.splice(index, 1);
    if (timer) clearTimeout(timer);
    reject(abortError());
  };
  signal?.addEventListener("abort", onAbort, { once: true });
  const wrappedResolve = waiter.resolve;
  waiter.resolve = (lease) => {
    if (timer) clearTimeout(timer);
    signal?.removeEventListener("abort", onAbort);
    wrappedResolve(lease);
  };
  const wrappedReject = waiter.reject;
  waiter.reject = (error) => {
    if (timer) clearTimeout(timer);
    signal?.removeEventListener("abort", onAbort);
    wrappedReject(error);
  };
  waiters.push(waiter);
  timer = setTimeout(() => {
    const index = waiters.indexOf(waiter);
    if (index >= 0) waiters.splice(index, 1);
    waiter.reject(timeoutError(Date.now() - waiter.startedAt));
  }, Math.max(1, timeoutMs));
  timer.unref?.();
  dispatch();
});

export const withCaptureWorker = async (options, callback) => {
  const lease = await acquireCaptureWorker(options);
  try {
    return await callback(lease);
  } finally {
    lease.release();
  }
};

export const getCaptureQueueSnapshot = () => ({ active, queued: waiters.length });
