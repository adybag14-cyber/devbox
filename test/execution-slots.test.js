import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { currentProcessInstance } from "../src/process-identity.js";

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




test("io-heavy capacity serializes recursive storage workloads without consuming all execution slots", async () => {
  const { acquireExecutionSlot, getExecutionSlotSnapshot } = await importIsolatedSlots();
  const first = await acquireExecutionSlot({
    kind: "interactive",
    resourceClass: "io-heavy",
    weight: 2,
    maxConcurrent: 4,
    reservedInteractive: 1,
    heavyCapacity: 4,
    ioHeavyCapacity: 2,
    queueTimeoutMs: 500,
    label: "io-heavy-first",
  });
  try {
    const snapshot = await getExecutionSlotSnapshot({
      maxConcurrent: 4,
      reservedInteractive: 1,
      heavyCapacity: 4,
      ioHeavyCapacity: 2,
    });
    assert.equal(snapshot.io_heavy_capacity, 2);
    assert.equal(first.weight, 2);
    assert.equal(snapshot.occupied, 2);
    await assert.rejects(
      acquireExecutionSlot({
        kind: "interactive",
        resourceClass: "io-heavy",
        weight: 2,
        maxConcurrent: 4,
        reservedInteractive: 1,
        heavyCapacity: 4,
        ioHeavyCapacity: 2,
        queueTimeoutMs: 120,
        label: "io-heavy-second",
      }),
      /Execution queue remained saturated/u,
    );
  } finally {
    await first.release();
  }
});



test("warning disk pressure serializes heavy workloads through the shared low-slot corridor", async () => {
  const { acquireExecutionSlot, testSlotRoot } = await importIsolatedSlots();
  await writeFile(path.join(testSlotRoot, ".disk-pressure.json"), JSON.stringify({ diskPressure: "warning" }));
  const first = await acquireExecutionSlot({
    kind: "interactive",
    resourceClass: "heavy",
    weight: 2,
    maxConcurrent: 4,
    reservedInteractive: 1,
    heavyCapacity: 4,
    ioHeavyCapacity: 2,
    queueTimeoutMs: 500,
    label: "pressure-heavy-first",
  });
  try {
    assert.deepEqual(first.slots, [0, 1]);
    await assert.rejects(
      acquireExecutionSlot({
        kind: "interactive",
        resourceClass: "heavy",
        weight: 2,
        maxConcurrent: 4,
        reservedInteractive: 1,
        heavyCapacity: 4,
        ioHeavyCapacity: 2,
        queueTimeoutMs: 120,
        label: "pressure-heavy-second",
      }),
      /Execution queue remained saturated/u,
    );
  } finally {
    await first.release();
  }
});



test("disk pressure lets light interactive work bypass a blocked weighted waiter without stealing its corridor", async () => {
  const { acquireExecutionSlot, testSlotRoot } = await importIsolatedSlots();
  await writeFile(path.join(testSlotRoot, ".disk-pressure.json"), JSON.stringify({ diskPressure: "warning" }));
  const common = {
    maxConcurrent: 4,
    reservedInteractive: 1,
    heavyCapacity: 4,
    heavyWeight: 2,
    ioHeavyCapacity: 2,
    ioHeavyWeight: 2,
    queueTimeoutMs: 1000,
  };
  const first = await acquireExecutionSlot({
    ...common,
    kind: "interactive",
    resourceClass: "heavy",
    weight: 2,
    label: "pressure-weighted-first",
  });
  let second = null;
  let light = null;
  try {
    let secondResolved = false;
    const secondPromise = acquireExecutionSlot({
      ...common,
      kind: "interactive",
      resourceClass: "heavy",
      weight: 2,
      label: "pressure-weighted-second",
    }).then((lease) => {
      secondResolved = true;
      return lease;
    });
    await new Promise((resolve) => setTimeout(resolve, 40));
    const lightStarted = performance.now();
    light = await acquireExecutionSlot({
      ...common,
      kind: "interactive",
      resourceClass: "light",
      weight: 1,
      label: "pressure-light",
    });
    assert.ok(performance.now() - lightStarted < 400, "light request remained head-of-line blocked by weighted waiter");
    assert.ok(light.slot >= 2, `light request stole pressure weighted corridor slot ${light.slot}`);
    assert.equal(secondResolved, false);
    await light.release();
    light = null;
    await first.release();
    second = await secondPromise;
    assert.deepEqual(second.slots, [0, 1]);
  } finally {
    await light?.release().catch(() => {});
    await second?.release().catch(() => {});
    await first.release().catch(() => {});
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




test("aged background waiter gets a bounded priority turn ahead of new interactive work", async () => {
  const { acquireExecutionSlot } = await importIsolatedSlots();
  const blocker = await acquireExecutionSlot({
    kind: "interactive",
    maxConcurrent: 1,
    reservedInteractive: 0,
    backgroundPriorityAgeMs: 1,
    queueTimeoutMs: 1000,
    label: "aging-blocker",
  });
  let background = null;
  let interactive = null;
  try {
    const backgroundPromise = acquireExecutionSlot({
      kind: "background",
      maxConcurrent: 1,
      reservedInteractive: 0,
      backgroundPriorityAgeMs: 1,
      queueTimeoutMs: 1000,
      label: "aged-background",
    });
    await new Promise((resolve) => setTimeout(resolve, 30));
    let interactiveResolved = false;
    const interactivePromise = acquireExecutionSlot({
      kind: "interactive",
      maxConcurrent: 1,
      reservedInteractive: 0,
      backgroundPriorityAgeMs: 1,
      queueTimeoutMs: 1000,
      label: "later-interactive",
    }).then((lease) => {
      interactiveResolved = true;
      return lease;
    });
    await blocker.release();
    background = await backgroundPromise;
    await new Promise((resolve) => setTimeout(resolve, 40));
    assert.equal(interactiveResolved, false, "new interactive work bypassed aged background waiter");
    await background.release();
    background = null;
    interactive = await interactivePromise;
    assert.equal(interactive.kind, "interactive");
  } finally {
    await interactive?.release().catch(() => {});
    await background?.release().catch(() => {});
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
    processInstance: await currentProcessInstance(),
    class: queueClass,
    kind: "background",
    resourceClass: "light",
    weight: 1,
    label: "rust-protocol-fixture",
    sequence: sequence.toString(),
    queuedAtUnixMs: 1,
    queuedAtUtc: new Date(1).toISOString(),
    queueTimeoutMs: 1,
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
