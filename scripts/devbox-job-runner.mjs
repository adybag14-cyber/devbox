import { randomUUID } from "node:crypto";
import { access, readFile, rename, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

import { config } from "../src/config.js";
import { acquireExecutionSlot } from "../src/execution-slots.js";
import { createRotatingFileSink } from "../src/job-logs.js";
import { execInDevbox, execReadOnlyInDevbox, runProgramInDevbox } from "../src/runtime.js";

const requestPath = process.argv[2];
if (!requestPath) throw new Error("job runner requires a request path");
const dir = path.dirname(requestPath);
const statusPath = path.join(dir, "status.json");
const stdoutPath = path.join(dir, "stdout.log");
const stderrPath = path.join(dir, "stderr.log");
const cancelPath = path.join(dir, "cancel.requested");
const heartbeatPath = path.join(dir, "heartbeat.json");

const writeStatus = async (value) => {
  const temp = `${statusPath}.${process.pid}.tmp`;
  await writeFile(temp, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  await rename(temp, statusPath);
};

const writeHeartbeat = async (state) => {
  const temp = `${heartbeatPath}.${process.pid}.${randomUUID()}.tmp`;
  const payload = `${JSON.stringify({
    pid: process.pid,
    status: state,
    childPid,
    runtimeMode: request.runtimeMode || config.runtimeMode,
    updatedAtUtc: new Date().toISOString(),
  })}
`;
  await writeFile(temp, payload, "utf8");
  try {
    await rename(temp, heartbeatPath);
  } catch (error) {
    if (!["EEXIST", "EPERM"].includes(error?.code)) throw error;
    try {
      await writeFile(heartbeatPath, payload, "utf8");
    } finally {
      await rm(temp, { force: true }).catch(() => {});
    }
  }
};

const readStatus = () => readFile(statusPath, "utf8").then(JSON.parse).catch(() => null);
const request = JSON.parse(await readFile(requestPath, "utf8"));
const controller = new AbortController();
let childPid = null;
for (const signal of ["SIGINT", "SIGTERM"]) {
  process.once(signal, () => controller.abort());
}

const isCancellationRequested = async () => {
  try {
    await access(cancelPath);
    return true;
  } catch (error) {
    if (error?.code === "ENOENT") return false;
    throw error;
  }
};

const throwIfCancellationRequested = async () => {
  if (!(await isCancellationRequested())) return;
  controller.abort();
  const error = new Error("Background job cancellation was requested.");
  error.name = "AbortError";
  throw error;
};

const initial = await readStatus();
if (initial?.status === "cancelled" || await isCancellationRequested()) process.exit(0);

let heartbeatState = "queued";
await writeHeartbeat(heartbeatState).catch(() => {});
const heartbeatTimer = setInterval(() => writeHeartbeat(heartbeatState).catch(() => {}), Math.max(1000, config.mcpJobHeartbeatMs));
heartbeatTimer.unref?.();
const cancellationPoll = setInterval(() => {
  isCancellationRequested().then((requested) => {
    if (requested) controller.abort();
  }).catch(() => {});
}, 250);
cancellationPoll.unref?.();

const queuedAtUtc = new Date().toISOString();
await writeStatus({
  id: request.id,
  status: "queued",
  mode: request.mode || "shell",
  createdAtUtc: request.createdAtUtc,
  queuedAtUtc,
  startedAtUtc: null,
  completedAtUtc: null,
  runnerPid: process.pid,
  exitCode: null,
  readOnly: request.readOnly === true,
  resourceClass: request.resourceClass || "light",
  runtimeMode: request.runtimeMode || config.runtimeMode,
});
await throwIfCancellationRequested();

const stdout = createRotatingFileSink(stdoutPath, {
  maxBytes: config.mcpJobLogMaxBytes,
  rotations: config.mcpJobLogRotations,
});
const stderr = createRotatingFileSink(stderrPath, {
  maxBytes: config.mcpJobLogMaxBytes,
  rotations: config.mcpJobLogRotations,
});
let lease = null;
let finalStatus = null;

const resourceClass = request.resourceClass || "light";
const weight = resourceClass === "heavy" ? Math.max(1, config.mcpExecHeavyWeight) : 1;

try {
  lease = await acquireExecutionSlot({
    kind: "background",
    resourceClass,
    weight,
    label: `devbox_job:${request.id}`,
    maxConcurrent: config.mcpExecMaxConcurrent,
    reservedInteractive: config.mcpExecReservedInteractive,
    watchMaxConcurrent: config.mcpWatchMaxConcurrent,
    queueTimeoutMs: config.mcpBackgroundQueueTimeoutMs,
    signal: controller.signal,
  });

  await throwIfCancellationRequested();
  heartbeatState = "running";
  await writeHeartbeat(heartbeatState).catch(() => {});
  const startedAtUtc = new Date().toISOString();
  const base = {
    id: request.id,
    status: "running",
    mode: request.mode || "shell",
    createdAtUtc: request.createdAtUtc,
    queuedAtUtc,
    startedAtUtc,
    completedAtUtc: null,
    runnerPid: process.pid,
    exitCode: null,
    readOnly: request.readOnly === true,
    resourceClass,
    runtimeMode: request.runtimeMode || config.runtimeMode,
    queueWaitMs: lease.queueWaitMs,
    executionSlot: lease.slot,
    executionSlots: lease.slots,
    executionPool: lease.pool,
    executionWeight: lease.weight,
    childPid,
  };
  await writeStatus(base);
  await throwIfCancellationRequested();

  let result;
  if (request.mode === "program") {
    result = await runProgramInDevbox({
      program: request.program,
      args: Array.isArray(request.args) ? request.args : [],
      input: request.input,
      workingDir: request.workingDir || undefined,
      timeoutMs: request.timeoutMs,
      user: request.user || undefined,
      signal: controller.signal,
      onStdout: (text) => stdout.write(text),
      onStderr: (text) => stderr.write(text),
      maxCaptureChars: 65536,
      onSpawn: (pid) => {
        childPid = Number.isInteger(pid) && pid > 0 ? pid : null;
        writeHeartbeat(heartbeatState).catch(() => {});
      },
    });
  } else {
    const execute = request.readOnly ? execReadOnlyInDevbox : execInDevbox;
    result = await execute({
      command: request.command,
      workingDir: request.workingDir || undefined,
      timeoutMs: request.timeoutMs,
      user: request.user || undefined,
      signal: controller.signal,
      onStdout: (text) => stdout.write(text),
      onStderr: (text) => stderr.write(text),
      maxCaptureChars: 65536,
      onSpawn: (pid) => {
        childPid = Number.isInteger(pid) && pid > 0 ? pid : null;
        writeHeartbeat(heartbeatState).catch(() => {});
      },
    });
  }
  finalStatus = {
    ...base,
    childPid,
    status: "succeeded",
    completedAtUtc: new Date().toISOString(),
    exitCode: result.exitCode ?? 0,
  };
} catch (error) {
  const timedOut = error?.timedOut === true || /timed out/iu.test(String(error?.message ?? ""));
  const aborted = error?.aborted === true || error?.name === "AbortError" || controller.signal.aborted;
  const current = await readStatus();
  const externallyCancelled = await isCancellationRequested().catch(() => current?.status === "cancelled");
  finalStatus = {
    id: request.id,
    status: externallyCancelled || aborted ? "cancelled" : timedOut ? "timed_out" : "failed",
    mode: request.mode || "shell",
    createdAtUtc: request.createdAtUtc,
    queuedAtUtc,
    startedAtUtc: current?.startedAtUtc ?? null,
    completedAtUtc: new Date().toISOString(),
    runnerPid: process.pid,
    exitCode: error?.exitCode ?? null,
    readOnly: request.readOnly === true,
    resourceClass,
    runtimeMode: request.runtimeMode || config.runtimeMode,
    queueWaitMs: lease?.queueWaitMs ?? null,
    executionSlot: lease?.slot ?? null,
    executionSlots: lease?.slots ?? null,
    executionPool: lease?.pool ?? null,
    executionWeight: lease?.weight ?? weight,
    childPid,
    error: error instanceof Error ? error.message : String(error),
  };
} finally {
  clearInterval(cancellationPoll);
  clearInterval(heartbeatTimer);
  await lease?.release().catch(() => {});
  stdout.end();
  stderr.end();
}

const stdoutLog = stdout.snapshot();
const stderrLog = stderr.snapshot();
if (finalStatus) {
  finalStatus.logs = {
    stdout: stdoutLog,
    stderr: stderrLog,
    truncated: stdoutLog.truncated || stderrLog.truncated,
  };
}
heartbeatState = finalStatus?.status || "finished";
await writeHeartbeat(heartbeatState).catch(() => {});
const current = await readStatus();
if (current?.status !== "cancelled" && finalStatus) await writeStatus(finalStatus);
