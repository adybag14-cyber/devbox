import test from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";

import {
  currentProcessInstance,
  processAlive,
  processInstance,
  processMatchesInstance,
  processIdentityInternals,
} from "../src/process-identity.js";

test("process identity is stable for the current process and rejects a reused-instance token", async () => {
  const first = await currentProcessInstance();
  const second = await processInstance(process.pid);
  assert.notEqual(first, null);
  assert.equal(second, first);
  assert.equal(await processMatchesInstance(process.pid, first), true);
  assert.equal(await processMatchesInstance(process.pid, (BigInt(first) + 4096n).toString()), false);
});

test("identity-less and unsafe-number legacy owners remain compatible while dead PIDs are rejected", async () => {
  assert.equal(await processMatchesInstance(process.pid, null), true);
  assert.equal(await processMatchesInstance(process.pid, Number.MAX_SAFE_INTEGER + 2), true);
  assert.equal(await processMatchesInstance(2147483647, null), false);
});

test("EPERM liveness probes are treated as live on every platform", () => {
  const originalKill = process.kill;
  try {
    process.kill = () => {
      const error = new Error("permission denied");
      error.code = "EPERM";
      throw error;
    };
    assert.equal(processAlive(12345), true);
  } finally {
    process.kill = originalKill;
  }
});

test("remote ownership checks bypass stale PID-only identity cache entries", async () => {
  const child = spawn(process.execPath, ["-e", "setTimeout(()=>{},5000)"], { stdio: "ignore", windowsHide: true });
  try {
    const actual = await processInstance(child.pid, { bypassCache: true });
    assert.notEqual(actual, null);
    processIdentityInternals.cache.set(child.pid, { sampledAtMs: Date.now(), value: "stale-instance" });
    assert.equal(await processInstance(child.pid), "stale-instance");
    assert.equal(await processMatchesInstance(child.pid, actual), true);
  } finally {
    processIdentityInternals.cache.delete(child.pid);
    try { child.kill("SIGKILL"); } catch {}
  }
});
