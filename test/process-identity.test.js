import test from "node:test";
import assert from "node:assert/strict";

import { currentProcessInstance, processInstance, processMatchesInstance } from "../src/process-identity.js";

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
