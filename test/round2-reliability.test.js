import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, mkdir, readFile, rm, utimes, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { spawn } from "node:child_process";

import { abortableSleep, waitForPathCondition } from "../src/wait-utils.js";
import { shapeProcessOutput } from "../src/output-shaping.js";
import { createRotatingFileSink } from "../src/job-logs.js";

const projectRoot = process.cwd();

const importIsolatedSlots = async () => {
  process.env.MCP_EXEC_SLOT_ROOT = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-slots-"));
  const href = pathToFileURL(path.join(projectRoot, "src/execution-slots.js")).href;
  return import(`${href}?round2=${Date.now()}-${Math.random()}`);
};

test("passive watch jobs use a separate pool from execution jobs", async () => {
  const slots = await importIsolatedSlots();
  const watchA = await slots.acquireExecutionSlot({ kind: "background", resourceClass: "watch", maxConcurrent: 2, reservedInteractive: 1, watchMaxConcurrent: 2, queueTimeoutMs: 500 });
  const watchB = await slots.acquireExecutionSlot({ kind: "background", resourceClass: "watch", maxConcurrent: 2, reservedInteractive: 1, watchMaxConcurrent: 2, queueTimeoutMs: 500 });
  try {
    assert.equal(watchA.pool, "watch");
    assert.equal(watchB.pool, "watch");
    const normal = await slots.acquireExecutionSlot({ kind: "background", resourceClass: "light", maxConcurrent: 2, reservedInteractive: 1, watchMaxConcurrent: 2, queueTimeoutMs: 500 });
    try {
      assert.equal(normal.pool, "execution");
      const snapshot = await slots.getExecutionSlotSnapshot({ maxConcurrent: 2, reservedInteractive: 1, watchMaxConcurrent: 2 });
      assert.equal(snapshot.watch_occupied, 2);
      assert.equal(snapshot.occupied, 1);
    } finally {
      await normal.release();
    }
  } finally {
    await watchA.release();
    await watchB.release();
  }
});

test("heavy jobs consume weighted execution capacity while preserving the reserved interactive slot", async () => {
  const slots = await importIsolatedSlots();
  const heavy = await slots.acquireExecutionSlot({ kind: "background", resourceClass: "heavy", weight: 2, maxConcurrent: 4, reservedInteractive: 1, queueTimeoutMs: 500 });
  try {
    assert.equal(heavy.weight, 2);
    assert.equal(heavy.slots.length, 2);
    const light = await slots.acquireExecutionSlot({ kind: "background", resourceClass: "light", maxConcurrent: 4, reservedInteractive: 1, queueTimeoutMs: 500 });
    try {
      assert.equal(light.slots.length, 1);
      const interactive = await slots.acquireExecutionSlot({ kind: "interactive", resourceClass: "light", maxConcurrent: 4, reservedInteractive: 1, queueTimeoutMs: 500 });
      try {
        assert.equal(interactive.slots.length, 1);
        const snapshot = await slots.getExecutionSlotSnapshot({ maxConcurrent: 4, reservedInteractive: 1 });
        assert.equal(snapshot.occupied, 4);
      } finally {
        await interactive.release();
      }
    } finally {
      await light.release();
    }
  } finally {
    await heavy.release();
  }
});

test("abortableSleep waits without a process and responds promptly to cancellation", async () => {
  const controller = new AbortController();
  const started = Date.now();
  setTimeout(() => controller.abort(), 30);
  await assert.rejects(abortableSleep(5000, controller.signal), /cancelled/iu);
  assert.ok(Date.now() - started < 1000);
});

test("waitForPathCondition detects a file without shell polling", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-wait-"));
  const file = path.join(dir, "ready.txt");
  try {
    setTimeout(() => writeFile(file, "ready", "utf8"), 40);
    const result = await waitForPathCondition({ path: file, timeoutMs: 2000, pollMs: 25, minBytes: 5 });
    assert.equal(result.conditionMet, true);
    assert.equal(result.exists, true);
    assert.ok(result.waitedMs < 1500);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("output shaping supports head, tail, and summary modes", () => {
  const value = Array.from({ length: 20 }, (_v, i) => `line-${String(i).padStart(2, "0")}`).join("\n");
  const head = shapeProcessOutput(value, { mode: "head", maxChars: 1000, maxLines: 4 });
  const tail = shapeProcessOutput(value, { mode: "tail", maxChars: 1000, maxLines: 4 });
  const summary = shapeProcessOutput(value, { mode: "summary", maxChars: 1000, maxLines: 4 });
  assert.match(head.text, /^line-00/u);
  assert.doesNotMatch(head.text, /line-19/u);
  assert.match(tail.text, /line-19$/u);
  assert.doesNotMatch(tail.text, /line-00/u);
  assert.match(summary.text, /line-00/u);
  assert.match(summary.text, /line-19/u);
  assert.equal(head.truncated, true);
  assert.equal(summary.originalChars, value.length);
});

test("rotating job logs bound every segment and retain recent output", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-log-"));
  const file = path.join(dir, "stdout.log");
  try {
    const sink = createRotatingFileSink(file, { maxBytes: 4096, rotations: 2 });
    sink.write("A".repeat(5000));
    sink.write("TAIL-MARKER-" + "B".repeat(5000));
    sink.end();
    const snapshot = sink.snapshot();
    assert.equal(snapshot.truncated, true);
    for (const candidate of [file, `${file}.1`, `${file}.2`]) {
      const bytes = await readFile(candidate).then((b) => b.length).catch(() => 0);
      assert.ok(bytes <= 4096);
    }
    assert.match(await readFile(file, "utf8"), /B+$/u);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("orphaned running jobs reconcile to interrupted and resource classes are inferred", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-jobs-"));
  process.env.MCP_JOBS_ROOT = jobsRoot;
  const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
  const jobs = await import(`${href}?orphan=${Date.now()}-${Math.random()}`);
  assert.equal(jobs.inferJobResourceClass({ command: "gh run watch 123 --interval 30" }), "watch");
  assert.equal(jobs.inferJobResourceClass({ command: "node scripts/pioneer-playwright-workload.mjs" }), "heavy");
  assert.equal(jobs.inferJobResourceClass({ program: "git", args: ["status"] }), "light");
  const id = `job-test-${Date.now().toString(36)}-orphan99`;
  const paths = jobs.asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const old = new Date(Date.now() - 60000).toISOString();
  await writeFile(paths.status, `${JSON.stringify({ id, status: "running", runnerPid: 99999999, startedAtUtc: old, createdAtUtc: old })}\n`, "utf8");
  const status = await jobs.getDevboxJobStatus(id);
  assert.equal(status.status, "interrupted");
  assert.equal(status.runnerAlive, false);
  const persisted = JSON.parse(await readFile(paths.status, "utf8"));
  assert.equal(persisted.status, "interrupted");
  await rm(jobsRoot, { recursive: true, force: true });
});

test("Guardian Windows host pressure sampler returns bounded diagnostic telemetry", { skip: process.platform !== "win32" }, async () => {
  const href = pathToFileURL(path.join(projectRoot, "scripts/devbox-guardian.mjs")).href;
  const guardian = await import(`${href}?pressure=${Date.now()}-${Math.random()}`);
  const data = await guardian.sampleWindowsHostPressure(process.env);
  assert.ok(data.SampledAtUtc);
  assert.equal(typeof data.CpuPercent === "number" || typeof data.Error === "string", true);
  if (!data.Error) {
    assert.ok(data.TotalPhysicalBytes > 0);
    assert.ok(data.FreePhysicalBytes >= 0);
  }
});


test("screen capture policy retries one transient timeout and records retry metadata", async () => {
  const href = pathToFileURL(path.join(projectRoot, "src/screen-capture.js")).href;
  const { screenCaptureInternals } = await import(`${href}?capture-retry=${Date.now()}-${Math.random()}`);
  let calls = 0;
  const capture = await screenCaptureInternals.withCapturePolicy({ timeoutMs: 1000 }, async () => {
    calls += 1;
    if (calls === 1) {
      const error = new Error("Command timed out after 1000 ms.");
      error.timedOut = true;
      throw error;
    }
    return { image: Buffer.from([1, 2, 3]), mimeType: "image/jpeg", metadata: { fixture: true } };
  });
  assert.equal(calls, 2);
  assert.equal(capture.metadata.capture_attempts, 2);
  assert.equal(capture.metadata.capture_retried, true);
});

test("screen capture policy does not retry a non-transient correctness error", async () => {
  const href = pathToFileURL(path.join(projectRoot, "src/screen-capture.js")).href;
  const { screenCaptureInternals } = await import(`${href}?capture-hard=${Date.now()}-${Math.random()}`);
  let calls = 0;
  await assert.rejects(
    screenCaptureInternals.withCapturePolicy({ timeoutMs: 1000 }, async () => {
      calls += 1;
      throw new Error("No visible window belongs to this PID.");
    }),
    /No visible window/u,
  );
  assert.equal(calls, 1);
});


test("legacy oversized terminal logs can be compacted to the configured segment cap", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-legacy-log-"));
  const file = path.join(dir, "stderr.log");
  try {
    await writeFile(file, `HEAD${"X".repeat(9000)}TAIL`, "utf8");
    const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-legacy-jobs-"));
    process.env.MCP_JOBS_ROOT = jobsRoot;
    const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
    const jobs = await import(`${href}?compact=${Date.now()}-${Math.random()}`);
    const result = await jobs.asyncJobsInternals.compactLegacyLogFile(file, 4096);
    assert.equal(result.compacted, true);
    const compacted = await readFile(file, "utf8");
    assert.equal(Buffer.byteLength(compacted), 4096);
    assert.match(compacted, /TAIL$/u);
    await rm(jobsRoot, { recursive: true, force: true });
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});


test("orphan reconciliation terminates a surviving detached child process tree", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-orphan-child-"));
  process.env.MCP_JOBS_ROOT = jobsRoot;
  const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
  const jobs = await import(`${href}?orphan-child=${Date.now()}-${Math.random()}`);
  const child = spawn(process.execPath, ["-e", "setInterval(()=>{},1000)"], {
    cwd: projectRoot,
    stdio: "ignore",
    windowsHide: true,
  });
  const childPid = child.pid;
  assert.ok(Number.isInteger(childPid) && childPid > 0);
  const id = `job-test-${Date.now().toString(36)}-child123`;
  const paths = jobs.asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const old = new Date(Date.now() - 60000);
  await writeFile(paths.status, `${JSON.stringify({ id, status: "running", runnerPid: 99999999, childPid, createdAtUtc: old.toISOString(), startedAtUtc: old.toISOString() })}\n`, "utf8");
  await writeFile(paths.heartbeat, `${JSON.stringify({ pid: 99999999, status: "running", childPid, updatedAtUtc: old.toISOString() })}\n`, "utf8");
  await utimes(paths.heartbeat, old, old);
  try {
    const status = await jobs.getDevboxJobStatus(id);
    assert.equal(status.status, "interrupted");
    assert.equal(status.childPid, childPid);
    assert.equal(status.orphanChildTerminated, true);
    await new Promise((resolve) => setTimeout(resolve, 150));
    assert.equal(jobs.asyncJobsInternals.processAlive(childPid), false);
  } finally {
    if (jobs.asyncJobsInternals.processAlive(childPid)) {
      try { child.kill("SIGKILL"); } catch {}
    }
    await rm(jobsRoot, { recursive: true, force: true });
  }
});
