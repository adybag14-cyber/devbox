import test from "node:test";
import assert from "node:assert/strict";
import { mkdir, rm, writeFile } from "node:fs/promises";

import { asyncJobsInternals, getDevboxJobLogs, getDevboxJobStatus, reconcileOrphanedDevboxJobs } from "../src/async-jobs.js";

test("async job ids reject traversal and accept generated-style ids", () => {
  assert.throws(() => asyncJobsInternals.assertJobId("../escape"), /Invalid Devbox job id/u);
  assert.throws(() => asyncJobsInternals.assertJobId("job/x"), /Invalid Devbox job id/u);
  assert.equal(asyncJobsInternals.assertJobId("job-test-12345678"), "job-test-12345678");
});

test("async job status and logs are persisted and bounded", async () => {
  const id = `job-test-${Date.now().toString(36)}-abcdef12`;
  const paths = asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  try {
    await writeFile(paths.status, `${JSON.stringify({ id, status: "succeeded", runnerPid: null, exitCode: 0 })}\n`, "utf8");
    await writeFile(paths.stdout, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", "utf8");
    await writeFile(paths.stderr, "stderr-tail", "utf8");
    const status = await getDevboxJobStatus(id);
    assert.equal(status.status, "succeeded");
    assert.equal(status.runnerAlive, false);
    const logs = await getDevboxJobLogs({ jobId: id, maxChars: 12 });
    // Public API enforces a 100-character minimum, so the complete short fixture remains available.
    assert.match(logs.stdout, /ABCDEFGHIJKLMNOPQRSTUVWXYZ/u);
    assert.equal(logs.stderr, "stderr-tail");
  } finally {
    await rm(paths.dir, { recursive: true, force: true });
  }
});


test("runner instance token prevents a reused live PID from inheriting job ownership", async () => {
  const id = `job-test-${Date.now().toString(36)}-reuse123`;
  const paths = asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  try {
    const status = {
      id,
      status: "running",
      runnerPid: process.pid,
      runnerProcessInstance: "original-runner",
      createdAtUtc: new Date().toISOString(),
    };
    await writeFile(paths.status, `${JSON.stringify(status)}\n`, "utf8");
    await writeFile(paths.heartbeat, `${JSON.stringify({
      pid: process.pid,
      runnerProcessInstance: "reused-pid-runner",
      updatedAtUtc: new Date().toISOString(),
    })}\n`, "utf8");
    const owner = await asyncJobsInternals.runnerOwnerState(paths, status);
    assert.equal(owner.processPresent, true);
    assert.equal(owner.instanceMatches, false);
    assert.equal(owner.alive, false);
  } finally {
    await rm(paths.dir, { recursive: true, force: true });
  }
});

test("cancellation marker is authoritative over a racing queued/running status write", async () => {
  const id = `job-test-${Date.now().toString(36)}-cancel12`;
  const paths = asyncJobsInternals.jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  try {
    await writeFile(paths.status, `${JSON.stringify({ id, status: "running", runnerPid: process.pid, exitCode: null })}\n`, "utf8");
    await writeFile(paths.cancel, `${new Date().toISOString()}\n`, "utf8");
    const status = await getDevboxJobStatus(id);
    assert.equal(status.status, "cancelled");
    assert.equal(status.cancelRequested, true);
    assert.equal(status.runnerAlive, true);
  } finally {
    await rm(paths.dir, { recursive: true, force: true });
  }
});

test("periodic async-job maintenance reports global store quota accounting", async () => {
  const ids = [0, 1].map((index) => `job-test-${Date.now().toString(36)}-quota${index}x`);
  try {
    for (const id of ids) {
      const paths = asyncJobsInternals.jobPaths(id);
      await mkdir(paths.dir, { recursive: true });
      await writeFile(paths.status, `${JSON.stringify({
        id,
        status: "succeeded",
        runnerPid: null,
        createdAtUtc: new Date().toISOString(),
        completedAtUtc: new Date().toISOString(),
      })}\n`, "utf8");
      await writeFile(paths.stdout, "x".repeat(1024), "utf8");
    }
    const summary = await reconcileOrphanedDevboxJobs();
    assert.ok(summary.storeBytes >= 2 * 1024);
    assert.ok(summary.terminalRetained >= 2);
    assert.equal(typeof summary.quotaPressure, "boolean");
    assert.match(summary.quotaCheckedAtUtc, /^\d{4}-\d{2}-\d{2}T/u);
  } finally {
    await Promise.all(ids.map((id) => rm(asyncJobsInternals.jobPaths(id).dir, { recursive: true, force: true })));
  }
});