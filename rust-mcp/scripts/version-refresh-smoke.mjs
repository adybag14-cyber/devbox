import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";

export const versionSmokeCacheMs = 31_000;

// Observe the supervised refresh; status must remain a cache-only operation.
// A fixed sleep cannot guarantee freshness when probe duration varies.
export async function waitForVersionRefresh(readStatus, {
  newerThan = 0,
  timeoutMs = 90_000,
  pollMs = 250,
  now = () => performance.now(),
  wait = delay,
} = {}) {
  assert(Number.isFinite(timeoutMs) && timeoutMs > 0);
  assert(Number.isFinite(pollMs) && pollMs > 0);
  const deadline = now() + timeoutMs;
  let observed = null;
  while (now() < deadline) {
    const response = await readStatus(Math.max(1, Math.min(5_000, deadline - now())));
    assert.equal(response.isError, false, "Version status request must succeed");
    assert.equal(response.structuredContent?.ok, true, "Version status envelope must succeed");
    const data = response.structuredContent.data;
    const refresh = data?.backgroundTasks?.["version-refresh"];
    observed = {
      cached: data?.versionsCached,
      versionCount: Array.isArray(data?.versions) ? data.versions.length : null,
      running: refresh?.running,
      failures: refresh?.consecutiveFailures,
      lastSuccessUnixMs: refresh?.lastSuccessUnixMs,
      lastError: refresh?.lastError,
    };
    assert.notEqual(refresh?.running, false, `Version refresher stopped: ${JSON.stringify(observed)}`);
    assert.equal(refresh?.consecutiveFailures ?? 0, 0, `Version refresher failed: ${JSON.stringify(observed)}`);
    if (now() < deadline && data?.versionsCached === true && Array.isArray(data.versions)
      && refresh?.running === true && Number.isFinite(refresh.lastSuccessUnixMs)
      && refresh.lastSuccessUnixMs > newerThan) return response;
    await wait(Math.max(0, Math.min(pollMs, deadline - now())));
  }
  throw new Error(`No fresh supervised version publication before the deadline: ${JSON.stringify(observed)}`);
}
