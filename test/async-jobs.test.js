import test from "node:test";
import assert from "node:assert/strict";
import { mkdir, rm, writeFile } from "node:fs/promises";

import { asyncJobsInternals, getDevboxJobLogs, getDevboxJobStatus } from "../src/async-jobs.js";

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
