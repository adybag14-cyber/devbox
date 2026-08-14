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
import { refreshExecutionStoreHealth } from "../src/execution-store-health.js";

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
  const old = new Date(Date.now() - 20000);
  await writeFile(paths.status, `${JSON.stringify({ id, status: "running", runnerPid: 99999999, childPid, createdAtUtc: old.toISOString(), startedAtUtc: old.toISOString() })}\n`, "utf8");
  await writeFile(paths.heartbeat, `${JSON.stringify({ pid: 99999999, status: "running", childPid, updatedAtUtc: old.toISOString() })}\n`, "utf8");
  await utimes(paths.heartbeat, old, old);
  try {
    const status = await jobs.getDevboxJobStatus(id);
    assert.equal(status.status, "interrupted");
    assert.equal(status.childPid, childPid);
    assert.equal(status.orphanChildTerminated, true);
    const reapDeadline = Date.now() + 3000;
    while (Date.now() < reapDeadline && jobs.asyncJobsInternals.processAlive(childPid)) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    assert.equal(jobs.asyncJobsInternals.processAlive(childPid), false);
  } finally {
    if (jobs.asyncJobsInternals.processAlive(childPid)) {
      try { child.kill("SIGKILL"); } catch {}
    }
    await rm(jobsRoot, { recursive: true, force: true });
  }
});


test("old orphan heartbeats never kill a live PID that may have been reused", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-old-orphan-child-"));
  process.env.MCP_JOBS_ROOT = jobsRoot;
  const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
  const jobs = await import(`${href}?old-orphan-child=${Date.now()}-${Math.random()}`);
  const child = spawn(process.execPath, ["-e", "setInterval(()=>{},1000)"], {
    cwd: projectRoot,
    stdio: "ignore",
    windowsHide: true,
  });
  const childPid = child.pid;
  const id = `job-test-${Date.now().toString(36)}-oldchild`;
  const paths = jobs.asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const old = new Date(Date.now() - 120000);
  await writeFile(paths.status, `${JSON.stringify({ id, status: "running", runnerPid: 99999999, childPid, createdAtUtc: old.toISOString(), startedAtUtc: old.toISOString() })}\n`, "utf8");
  await writeFile(paths.heartbeat, `${JSON.stringify({ pid: 99999999, status: "running", childPid, updatedAtUtc: old.toISOString() })}\n`, "utf8");
  await utimes(paths.heartbeat, old, old);
  try {
    const status = await jobs.getDevboxJobStatus(id);
    assert.equal(status.status, "interrupted");
    assert.equal(status.orphanChildTerminated, false);
    assert.equal(status.orphanChildCleanupSkipped, "heartbeat-too-old-to-safely-trust-reused-pid");
    assert.equal(jobs.asyncJobsInternals.processAlive(childPid), true);
  } finally {
    if (jobs.asyncJobsInternals.processAlive(childPid)) {
      try { child.kill("SIGKILL"); } catch {}
    }
    await rm(jobsRoot, { recursive: true, force: true });
  }
});


test("concurrent weighted jobs serialize claims instead of livelocking on partial slots", async () => {
  const slots = await importIsolatedSlots();
  const acquired = [];
  const runHeavy = async (label) => {
    const lease = await slots.acquireExecutionSlot({
      kind: "background",
      resourceClass: "heavy",
      weight: 2,
      maxConcurrent: 3,
      reservedInteractive: 0,
      queueTimeoutMs: 3000,
      label,
    });
    acquired.push({ label, slots: lease.slots, at: Date.now() });
    await new Promise((resolve) => setTimeout(resolve, 120));
    await lease.release();
  };
  await Promise.all([runHeavy("heavy-a"), runHeavy("heavy-b")]);
  assert.equal(acquired.length, 2);
  assert.equal(acquired[0].slots.length, 2);
  assert.equal(acquired[1].slots.length, 2);
  assert.ok(Math.abs(acquired[1].at - acquired[0].at) >= 80);
});

test("terminal job retention removes directories older than the configured default window", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-retention-"));
  process.env.MCP_JOBS_ROOT = jobsRoot;
  const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
  const jobs = await import(`${href}?retention=${Date.now()}-${Math.random()}`);
  const id = `job-test-${Date.now().toString(36)}-retained`;
  const paths = jobs.asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const old = new Date(Date.now() - 8 * 24 * 60 * 60 * 1000).toISOString();
  await writeFile(paths.status, `${JSON.stringify({ id, status: "succeeded", createdAtUtc: old, completedAtUtc: old, exitCode: 0 })}\n`, "utf8");
  await writeFile(paths.stdout, "old stdout", "utf8");
  await writeFile(paths.stderr, "old stderr", "utf8");
  const summary = await jobs.reconcileOrphanedDevboxJobs();
  assert.equal(summary.deleted, 1);
  assert.equal(await readFile(paths.status, "utf8").then(() => true).catch(() => false), false);
  await rm(jobsRoot, { recursive: true, force: true });
});

test("Docker orphan cleanup terminates only the local docker client identity and never claims the shared container was killed", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-docker-orphan-"));
  process.env.MCP_JOBS_ROOT = jobsRoot;
  const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
  const jobs = await import(`${href}?docker-orphan=${Date.now()}-${Math.random()}`);
  const child = spawn(process.execPath, ["-e", "setInterval(()=>{},1000)"], { cwd: projectRoot, stdio: "ignore", windowsHide: true });
  const childPid = child.pid;
  const id = `job-test-${Date.now().toString(36)}-docker12`;
  const paths = jobs.asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const old = new Date(Date.now() - 20000);
  await writeFile(paths.status, `${JSON.stringify({ id, status: "running", runtimeMode: "docker", runnerPid: 99999999, childPid, createdAtUtc: old.toISOString(), startedAtUtc: old.toISOString() })}\n`, "utf8");
  await writeFile(paths.heartbeat, `${JSON.stringify({ pid: 99999999, status: "running", runtimeMode: "docker", childPid, updatedAtUtc: old.toISOString() })}\n`, "utf8");
  await utimes(paths.heartbeat, old, old);
  try {
    const status = await jobs.getDevboxJobStatus(id);
    assert.equal(status.status, "interrupted");
    assert.equal(status.runtimeMode, "docker");
    assert.equal(status.orphanDockerClientTerminated, true);
    assert.equal(status.orphanChildTerminated, false);
    assert.equal(status.orphanChildCleanupSkipped, "docker-container-exec-not-force-killed-shared-container");
  } finally {
    if (jobs.asyncJobsInternals.processAlive(childPid)) {
      try { child.kill("SIGKILL"); } catch {}
    }
    await rm(jobsRoot, { recursive: true, force: true });
  }
});

test("screen capture retries remain inside one caller timeout budget", async () => {
  const href = pathToFileURL(path.join(projectRoot, "src/screen-capture.js")).href;
  const { screenCaptureInternals } = await import(`${href}?capture-budget=${Date.now()}-${Math.random()}`);
  let calls = 0;
  const started = Date.now();
  await assert.rejects(
    screenCaptureInternals.withCapturePolicy({ timeoutMs: 120 }, async () => {
      calls += 1;
      await new Promise((resolve) => setTimeout(resolve, 90));
      const error = new Error("Command timed out after the attempt budget.");
      error.timedOut = true;
      throw error;
    }),
    /timed out/iu,
  );
  const elapsed = Date.now() - started;
  assert.equal(calls, 1);
  assert.ok(elapsed < 350, `capture budget took ${elapsed} ms`);
});

test("line shaping preserves a terminal newline without counting it as an extra content line", () => {
  const shaped = shapeProcessOutput("one\ntwo\nthree\n", { mode: "tail", maxChars: 1000, maxLines: 2 });
  assert.equal(shaped.originalLines, 3);
  assert.match(shaped.text, /two\nthree\n$/u);
  assert.doesNotMatch(shaped.text, /one/u);
});


test("terminal job maintenance is marked and not repeated on every periodic reconciliation", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-maintenance-once-"));
  process.env.MCP_JOBS_ROOT = jobsRoot;
  const href = pathToFileURL(path.join(projectRoot, "src/async-jobs.js")).href;
  const jobs = await import(`${href}?maintenance-once=${Date.now()}-${Math.random()}`);
  const id = `job-test-${Date.now().toString(36)}-maintain`;
  const paths = jobs.asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const now = new Date().toISOString();
  await writeFile(paths.status, `${JSON.stringify({ id, status: "succeeded", createdAtUtc: now, completedAtUtc: now, exitCode: 0 })}\n`, "utf8");
  await writeFile(paths.stdout, "small stdout", "utf8");
  await writeFile(paths.stderr, "small stderr", "utf8");
  const first = await jobs.reconcileOrphanedDevboxJobs();
  const persisted = JSON.parse(await readFile(paths.status, "utf8"));
  const second = await jobs.reconcileOrphanedDevboxJobs();
  assert.equal(first.maintained, 1);
  assert.ok(persisted.maintenanceReconciledAtUtc);
  assert.equal(second.maintained, 0);
  await rm(jobsRoot, { recursive: true, force: true });
});


test("execution-store health reports warning pressure before the hard free-space floor", async () => {
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-disk-pressure-"));
  const common = {
    jobsRoot,
    probeWritablePath: async () => true,
    probeExecutionSlotStoreWritable: async () => true,
    minimumFreeBytes: 512 * 1024 * 1024,
    warningFreeBytes: 50 * 1024 * 1024 * 1024,
    warningFreePercent: 5,
  };
  const warning = await refreshExecutionStoreHealth({
    ...common,
    statfs: async () => ({ bavail: 220_000_000_000 / 4096, bsize: 4096, blocks: 8_000_000_000_000 / 4096 }),
  });
  assert.equal(warning.ok, true);
  assert.equal(warning.diskPressure, "warning");
  const critical = await refreshExecutionStoreHealth({
    ...common,
    statfs: async () => ({ bavail: 500_000_000 / 4096, bsize: 4096, blocks: 8_000_000_000_000 / 4096 }),
  });
  assert.equal(critical.ok, false);
  assert.equal(critical.diskPressure, "critical");
  const normal = await refreshExecutionStoreHealth({
    ...common,
    statfs: async () => ({ bavail: 800_000_000_000 / 4096, bsize: 4096, blocks: 8_000_000_000_000 / 4096 }),
  });
  assert.equal(normal.ok, true);
  assert.equal(normal.diskPressure, "normal");
  await rm(jobsRoot, { recursive: true, force: true });
});
