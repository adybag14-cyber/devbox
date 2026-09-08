import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import fsPromises, { mkdir, mkdtemp, readdir, rm, writeFile } from "node:fs/promises";
import { syncBuiltinESMExports } from "node:module";
import { pathToFileURL } from "node:url";
import { currentProcessInstance } from "../src/process-identity.js";

const importIsolatedSlots = async () => {
  // These tests exercise scheduler ordering/capacity, not cold PowerShell
  // startup. Establish the fixture's identity before starting short queue
  // deadlines; process-identity.test.js covers probe failures and recovery.
  const identityDeadline = Date.now() + 15000;
  let identity = await currentProcessInstance();
  while (identity === null && Date.now() < identityDeadline) {
    await new Promise(resolve => setTimeout(resolve, 100));
    identity = await currentProcessInstance();
  }
  assert.notEqual(identity, null, "scheduler fixture needs a verified current-process identity");
  const slotRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-exec-slots-test-"));
  process.env.MCP_EXEC_SLOT_ROOT = slotRoot;
  const href = pathToFileURL(path.join(process.cwd(), "src/execution-slots.js")).href;
  const module = await import(`${href}?slot=${Date.now()}-${Math.random()}`);
  return { ...module, testSlotRoot: slotRoot };
};

for (const target of ["slot-01.json", "execution-weighted-claim.json", ".execution-background-head.lock"]) {
  test(`Windows admission recovers from transient exclusive-open sharing errors: ${target}`, {
    skip: process.platform !== "win32",
  }, async (t) => {
    const { acquireExecutionSlot, getExecutionSlotSnapshot, testSlotRoot } = await importIsolatedSlots();
    const nativeOpen = fsPromises.open;
    let faults = 0;
    const mocked = t.mock.method(fsPromises, "open", async (file, flags, ...args) => {
      if (path.basename(String(file)) === target && flags === "wx" && faults < 3) {
        faults += 1;
        throw Object.assign(new Error("fixture: prior deletion is still pending"), { code: "EPERM" });
      }
      return nativeOpen(file, flags, ...args);
    });
    syncBuiltinESMExports();
    let lease;
    try {
      lease = await acquireExecutionSlot({ kind: "background", resourceClass: "heavy", weight: 2,
        maxConcurrent: 2, reservedInteractive: 0, queueTimeoutMs: 1500 });
      assert.equal(faults, 3);
      assert.equal(lease.slots.length, 2);
      assert.equal((await getExecutionSlotSnapshot({ maxConcurrent: 2, reservedInteractive: 0 })).occupied, 2);
      await lease.release();
      lease = null;
      assert.deepEqual((await readdir(testSlotRoot)).filter(name => /^slot-\d+\.json$/u.test(name)), []);
    } finally {
      mocked.mock.restore();
      syncBuiltinESMExports();
      await lease?.release();
      await rm(testSlotRoot, { recursive: true, force: true });
    }
  });
}

test("Windows admission preserves persistent permission errors and releases partial claims", {
  skip: process.platform !== "win32",
}, async (t) => {
  const { acquireExecutionSlot, testSlotRoot } = await importIsolatedSlots();
  const nativeOpen = fsPromises.open;
  const denied = Object.assign(new Error("fixture: persistent access denial"), { code: "EPERM" });
  let faults = 0;
  const mocked = t.mock.method(fsPromises, "open", async (file, flags, ...args) => {
    if (path.basename(String(file)) === "slot-01.json" && flags === "wx") {
      faults += 1;
      throw denied;
    }
    return nativeOpen(file, flags, ...args);
  });
  syncBuiltinESMExports();
  try {
    await assert.rejects(acquireExecutionSlot({ kind: "background", resourceClass: "heavy", weight: 2,
      maxConcurrent: 2, reservedInteractive: 0, queueTimeoutMs: 1500 }), error => error === denied);
    assert(faults > 1 && faults <= 6, "permission retry must be bounded");
    assert.deepEqual((await readdir(testSlotRoot)).filter(name => /^slot-\d+\.json$/u.test(name)), []);
    assert(!((await readdir(testSlotRoot)).includes("execution-weighted-claim.json")));
  } finally {
    mocked.mock.restore();
    syncBuiltinESMExports();
    await rm(testSlotRoot, { recursive: true, force: true });
  }
});

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
    const normalizedSnapshot = await getExecutionSlotSnapshot({
      maxConcurrent: 4,
      ioHeavyCapacity: 1,
      ioHeavyWeight: 2,
    });
    assert.equal(normalizedSnapshot.io_heavy_capacity, 2);
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
  const cancellation = new AbortController();
  const waitForQueued = async (count) => {
    const deadline = Date.now() + 5000;
    do {
      const snapshot = await getExecutionSlotSnapshot({ maxConcurrent: 2, reservedInteractive: 0, heavyCapacity: 2 });
      if (snapshot.global_queued === count) return snapshot;
      await new Promise(resolve => setTimeout(resolve, 25));
    } while (Date.now() < deadline);
    assert.fail(`Expected ${count} persisted FIFO waiters before continuing`);
  };
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
  let heavyPromise;
  let lightPromise;
  try {
    heavyPromise = acquireExecutionSlot({
      kind: "background",
      resourceClass: "heavy",
      weight: 2,
      maxConcurrent: 2,
      reservedInteractive: 0,
      heavyCapacity: 2,
      queueTimeoutMs: 10000,
      label: "fifo-heavy-first",
      signal: cancellation.signal,
    }).then(lease => { heavy = lease; return lease; });
    heavyPromise.catch(() => {});
    await waitForQueued(1);
    lightPromise = acquireExecutionSlot({
      kind: "background",
      resourceClass: "light",
      maxConcurrent: 2,
      reservedInteractive: 0,
      heavyCapacity: 2,
      queueTimeoutMs: 10000,
      label: "fifo-light-later",
      signal: cancellation.signal,
    }).then(lease => { light = lease; return lease; });
    lightPromise.catch(() => {});
    const queued = await waitForQueued(2);
    assert.equal(queued.global_queued, 2);
    assert.equal(queued.global_queued_by_class["execution-background"], 2);

    await blocker.release();
    heavy = await heavyPromise;
    let lightResolved = false;
    lightPromise.then(() => { lightResolved = true; }, () => {});
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(lightResolved, false, "later light request overtook the earlier heavy FIFO head");
    assert.equal(heavy.weight, 2);

    await heavy.release();
    heavy = null;
    light = await lightPromise;
    assert.equal(light.weight, 1);
  } finally {
    cancellation.abort();
    await Promise.allSettled([heavyPromise, lightPromise].filter(Boolean));
    await light?.release().catch(() => {});
    await heavy?.release().catch(() => {});
    await blocker.release().catch(() => {});
  }
});




test("aged background ticket from another pool does not block interactive execution", async () => {
  const { acquireExecutionSlot, testSlotRoot } = await importIsolatedSlots();
  const queueRoot = path.join(testSlotRoot, "queue");
  await mkdir(queueRoot, { recursive: true });
  const queueClass = "execution-background";
  const sequence = BigInt(Date.now()) * 1_000_000n;
  const name = `${queueClass}-${sequence.toString().padStart(32, "0")}-watchfixture.json`;
  const ticketPath = path.join(queueRoot, name);
  await writeFile(ticketPath, `${JSON.stringify({
    token: "watchfixture",
    pid: process.pid,
    processInstance: await currentProcessInstance(),
    class: queueClass,
    kind: "background",
    resourceClass: "watch",
    weight: 1,
    label: "legacy-watch-fixture",
    sequence: sequence.toString(),
    queuedAtUnixMs: 1,
    queuedAtUtc: new Date(1).toISOString(),
    queueTimeoutMs: 600_000,
  })}
`, "utf8");
  let lease = null;
  try {
    lease = await acquireExecutionSlot({
      kind: "interactive",
      resourceClass: "light",
      maxConcurrent: 4,
      reservedInteractive: 1,
      watchMaxConcurrent: 2,
      backgroundPriorityAgeMs: 1,
      queueTimeoutMs: 500,
      label: "execution-interactive-with-watch-fixture",
    });
    assert.equal(lease.pool, "execution");
  } finally {
    await lease?.release().catch(() => {});
    await rm(ticketPath, { force: true });
  }
});

test("disk pressure light interactive bypasses a non-overlapping aged heavy background waiter", async () => {
  const { acquireExecutionSlot, getExecutionSlotSnapshot, testSlotRoot } = await importIsolatedSlots();
  await writeFile(path.join(testSlotRoot, ".disk-pressure.json"), JSON.stringify({ diskPressure: "warning" }));
  const common = {
    maxConcurrent: 4,
    reservedInteractive: 1,
    heavyCapacity: 4,
    heavyWeight: 2,
    ioHeavyCapacity: 2,
    ioHeavyWeight: 2,
    backgroundPriorityAgeMs: 1,
    queueTimeoutMs: 10000,
  };
  const first = await acquireExecutionSlot({
    ...common,
    kind: "interactive",
    resourceClass: "heavy",
    weight: 2,
    label: "pressure-aging-holder",
  });
  let background = null;
  let light = null;
  let backgroundPromise = null;
  const backgroundController = new AbortController();
  try {
    backgroundPromise = acquireExecutionSlot({
      ...common,
      kind: "background",
      resourceClass: "heavy",
      weight: 2,
      label: "pressure-aged-heavy-background",
      signal: backgroundController.signal,
    }).then((lease) => { background = lease; return lease; });
    // Observe the published queue ticket before testing ordering. A fixed sleep
    // can race ticket creation on Windows and accidentally benchmark disk latency.
    backgroundPromise.catch(() => {});
    const fixtureDeadline = Date.now() + 5000;
    let snapshot = await getExecutionSlotSnapshot(common);
    while (!snapshot.global_queued && Date.now() < fixtureDeadline) {
      await new Promise((resolve) => setTimeout(resolve, 10));
      snapshot = await getExecutionSlotSnapshot(common);
    }
    assert.equal(snapshot.global_queued, 1, "heavy waiter must be queued before the light request");
    await new Promise((resolve) => setTimeout(resolve, 10));
    light = await acquireExecutionSlot({
      ...common,
      kind: "interactive",
      resourceClass: "light",
      weight: 1,
      queueTimeoutMs: 5000,
      label: "pressure-light-after-aged-heavy",
    });
    // The heavy holder stays leased until light acquires: yielding to its blocked
    // background waiter would time out regardless of host scheduling speed.
    assert.equal(background, null, "heavy background waiter acquired the occupied corridor");
    assert.ok(light.slot >= 2, `light request stole weighted corridor slot ${light.slot}`);
    await light.release();
    light = null;
    await first.release();
    background = await backgroundPromise;
    assert.deepEqual(background.slots, [0, 1]);
  } finally {
    backgroundController.abort();
    await backgroundPromise?.catch(() => {});
    await light?.release().catch(() => {});
    await background?.release().catch(() => {});
    await first.release().catch(() => {});
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
