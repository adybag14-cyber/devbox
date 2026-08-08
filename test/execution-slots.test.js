import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp } from "node:fs/promises";
import { pathToFileURL } from "node:url";

const importIsolatedSlots = async () => {
  process.env.MCP_EXEC_SLOT_ROOT = await mkdtemp(path.join(os.tmpdir(), "devbox-exec-slots-test-"));
  const href = pathToFileURL(path.join(process.cwd(), "src/execution-slots.js")).href;
  return import(`${href}?slot=${Date.now()}-${Math.random()}`);
};

test("background execution leaves the reserved interactive slot available", async () => {
  const { acquireExecutionSlot, getExecutionSlotSnapshot } = await importIsolatedSlots();
  const background = await acquireExecutionSlot({
    kind: "background",
    maxConcurrent: 2,
    reservedInteractive: 1,
    queueTimeoutMs: 1000,
    label: "test-background",
  });
  try {
    assert.equal(background.slot, 0);
    const interactive = await acquireExecutionSlot({
      kind: "interactive",
      maxConcurrent: 2,
      reservedInteractive: 1,
      queueTimeoutMs: 1000,
      label: "test-interactive",
    });
    try {
      assert.equal(interactive.slot, 1);
      const snapshot = await getExecutionSlotSnapshot({ maxConcurrent: 2, reservedInteractive: 1 });
      assert.equal(snapshot.occupied, 2);
      assert.equal(snapshot.background_capacity, 1);
    } finally {
      await interactive.release();
    }
  } finally {
    await background.release();
  }
});

test("background execution times out rather than consuming the reserved slot", async () => {
  const { acquireExecutionSlot } = await importIsolatedSlots();
  const background = await acquireExecutionSlot({
    kind: "background",
    maxConcurrent: 2,
    reservedInteractive: 1,
    queueTimeoutMs: 1000,
    label: "test-background-holder",
  });
  try {
    await assert.rejects(
      acquireExecutionSlot({
        kind: "background",
        maxConcurrent: 2,
        reservedInteractive: 1,
        queueTimeoutMs: 100,
        label: "test-background-waiter",
      }),
      /Execution queue remained saturated/u,
    );
  } finally {
    await background.release();
  }
});
