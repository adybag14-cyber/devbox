import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";

const importIsolatedSlots = async () => {
  const slotRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-exec-slots-test-"));
  process.env.MCP_EXEC_SLOT_ROOT = slotRoot;
  const href = pathToFileURL(path.join(process.cwd(), "src/execution-slots.js")).href;
  const module = await import(`${href}?slot=${Date.now()}-${Math.random()}`);
  return { ...module, testSlotRoot: slotRoot };
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


test("shared FIFO prevents a later light job from overtaking an earlier heavy waiter", async () => {
  const { acquireExecutionSlot, getExecutionSlotSnapshot } = await importIsolatedSlots();
  const blocker = await acquireExecutionSlot({
    kind: "background",
    resourceClass: "light",
    maxConcurrent: 2,
    reservedInteractive: 0,
    heavyCapacity: 2,
    queueTimeoutMs: 2000,
    label: "fifo-blocker",
  });
  let heavy;
  let light;
  try {
    const heavyPromise = acquireExecutionSlot({
      kind: "background",
      resourceClass: "heavy",
      weight: 2,
      maxConcurrent: 2,
      reservedInteractive: 0,
      heavyCapacity: 2,
      queueTimeoutMs: 2000,
      label: "fifo-heavy-first",
    });
    await new Promise((resolve) => setTimeout(resolve, 75));
    const lightPromise = acquireExecutionSlot({
      kind: "background",
      resourceClass: "light",
      maxConcurrent: 2,
      reservedInteractive: 0,
      heavyCapacity: 2,
      queueTimeoutMs: 2000,
      label: "fifo-light-later",
    });
    await new Promise((resolve) => setTimeout(resolve, 75));
    const queued = await getExecutionSlotSnapshot({ maxConcurrent: 2, reservedInteractive: 0, heavyCapacity: 2 });
    assert.equal(queued.global_queued, 2);
    assert.equal(queued.global_queued_by_class["execution-background"], 2);

    await blocker.release();
    heavy = await heavyPromise;
    let lightResolved = false;
    lightPromise.then(() => { lightResolved = true; });
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(lightResolved, false, "later light request overtook the earlier heavy FIFO head");
    assert.equal(heavy.weight, 2);

    await heavy.release();
    heavy = null;
    light = await lightPromise;
    assert.equal(light.weight, 1);
  } finally {
    await light?.release().catch(() => {});
    await heavy?.release().catch(() => {});
    await blocker.release().catch(() => {});
  }
});


test("JavaScript waiter honors a live Rust protocol ticket", async () => {
  const { acquireExecutionSlot, testSlotRoot } = await importIsolatedSlots();
  const queueRoot = path.join(testSlotRoot, "queue");
  await mkdir(queueRoot, { recursive: true });
  const queueClass = "execution-background";
  const sequence = BigInt(Date.now()) * 1_000_000n;
  const name = `${queueClass}-${sequence.toString().padStart(32, "0")}-rustfixture.json`;
  const ticketPath = path.join(queueRoot, name);
  await writeFile(ticketPath, `${JSON.stringify({
    token: "rustfixture",
    pid: process.pid,
    processInstance: "123456789",
    class: queueClass,
    kind: "background",
    resourceClass: "light",
    weight: 1,
    label: "rust-protocol-fixture",
    sequence: sequence.toString(),
    queuedAtUnixMs: Date.now(),
    queuedAtUtc: new Date().toISOString(),
    queueTimeoutMs: 2000,
  })}\n`, "utf8");
  await writeFile(path.join(queueRoot, `.${queueClass}-head.json`), `${JSON.stringify({ name })}\n`, "utf8");
  await assert.rejects(
    acquireExecutionSlot({
      kind: "background",
      resourceClass: "light",
      maxConcurrent: 1,
      reservedInteractive: 0,
      queueTimeoutMs: 120,
      label: "js-later",
    }),
    /Execution queue remained saturated/u,
  );
  await rm(ticketPath, { force: true });
  await rm(path.join(queueRoot, `.${queueClass}-head.json`), { force: true });
  const lease = await acquireExecutionSlot({
    kind: "background",
    resourceClass: "light",
    maxConcurrent: 1,
    reservedInteractive: 0,
    queueTimeoutMs: 1000,
    label: "js-after-rust",
  });
  await lease.release();
});
