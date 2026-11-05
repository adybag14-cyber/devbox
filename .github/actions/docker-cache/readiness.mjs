import { performance } from 'node:perf_hooks';
import { setTimeout as pause } from 'node:timers/promises';

// A restarting daemon can time out or refuse a probe before it becomes ready.
// Keep those retries within one monotonic budget, including both CLI queries.
// Hosted-runner service restart can return before Docker info answers. Allow
// one minute overall and ten seconds per mirror probe; never wait indefinitely.
export async function waitForRegistryCache(mirror, readMirrors, readVersion,
  { now = () => performance.now(), sleep = pause, timeoutMs = 60000 } = {}) {
  const deadline = now() + timeoutMs;
  let lastError;
  const remaining = () => deadline - now();
  const timeout = maximum => Math.max(1, Math.ceil(Math.min(maximum, remaining())));
  while (remaining() > 0) {
    try {
      const mirrors = readMirrors(timeout(10000));
      if (remaining() <= 0) break;
      if (Array.isArray(mirrors) && mirrors.some(value =>
        typeof value === 'string' && value.replace(/\/$/, '') === mirror)) {
        const version = readVersion(timeout(5000));
        if (remaining() > 0 && typeof version === 'string' && version.trim()) return version.trim();
      }
    } catch (error) {
      lastError = error;
    }
    if (remaining() > 0) await sleep(Math.min(250, remaining()));
  }
  throw new Error('Docker did not acknowledge the registry cache configuration before its deadline',
    { cause: lastError });
}
