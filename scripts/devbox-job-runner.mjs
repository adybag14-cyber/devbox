import { createWriteStream } from "node:fs";
import { access, readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

import { config } from "../src/config.js";
import { acquireExecutionSlot } from "../src/execution-slots.js";
import { execInDevbox, execReadOnlyInDevbox } from "../src/runtime.js";

const requestPath = process.argv[2];
if (!requestPath) throw new Error("job runner requires a request path");
const dir = path.dirname(requestPath);
const statusPath = path.join(dir, "status.json");
const stdoutPath = path.join(dir, "stdout.log");
const stderrPath = path.join(dir, "stderr.log");
const cancelPath = path.join(dir, "cancel.requested");

const writeStatus = async (value) => {
  const temp = `${statusPath}.${process.pid}.tmp`;
  await writeFile(temp, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  await rename(temp, statusPath);
};

const readStatus = () => readFile(statusPath, "utf8").then(JSON.parse).catch(() => null);
const request = JSON.parse(await readFile(requestPath, "utf8"));
const controller = new AbortController();
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

const cancellationPoll = setInterval(() => {
  isCancellationRequested().then((requested) => {
    if (requested) controller.abort();
  }).catch(() => {});
}, 50);
cancellationPoll.unref?.();

const initial = await readStatus();
if (initial?.status === "cancelled" || await isCancellationRequested()) process.exit(0);

const queuedAtUtc = new Date().toISOString();
await writeStatus({
  id: request.id,
  status: "queued",
  createdAtUtc: request.createdAtUtc,
  queuedAtUtc,
  startedAtUtc: null,
  completedAtUtc: null,
  runnerPid: process.pid,
  exitCode: null,
  readOnly: request.readOnly === true,
});
await throwIfCancellationRequested();

const stdout = createWriteStream(stdoutPath, { flags: "a" });
const stderr = createWriteStream(stderrPath, { flags: "a" });
let lease = null;
let finalStatus = null;

try {
  lease = await acquireExecutionSlot({
    kind: "background",
    label: `devbox_job:${request.id}`,
    maxConcurrent: config.mcpExecMaxConcurrent,
    reservedInteractive: config.mcpExecReservedInteractive,
    queueTimeoutMs: config.mcpBackgroundQueueTimeoutMs,
    signal: controller.signal,
  });

  await throwIfCancellationRequested();
  const startedAtUtc = new Date().toISOString();
  const base = {
    id: request.id,
    status: "running",
    createdAtUtc: request.createdAtUtc,
    queuedAtUtc,
    startedAtUtc,
    completedAtUtc: null,
    runnerPid: process.pid,
    exitCode: null,
    readOnly: request.readOnly === true,
    queueWaitMs: lease.queueWaitMs,
    executionSlot: lease.slot,
  };
  const current = await readStatus();
  if (current?.status === "cancelled") {
    controller.abort();
    const error = new Error("Background job was cancelled while waiting for an execution slot.");
    error.name = "AbortError";
    throw error;
  }
  await writeStatus(base);
  await throwIfCancellationRequested();

  const execute = request.readOnly ? execReadOnlyInDevbox : execInDevbox;
  const result = await execute({
    command: request.command,
    workingDir: request.workingDir || undefined,
    timeoutMs: request.timeoutMs,
    user: request.user || undefined,
    signal: controller.signal,
    onStdout: (text) => stdout.write(text),
    onStderr: (text) => stderr.write(text),
    maxCaptureChars: 65536,
  });
  finalStatus = {
    ...base,
    status: "succeeded",
    completedAtUtc: new Date().toISOString(),
    exitCode: result.exitCode ?? 0,
  };
} catch (error) {
  const timedOut = error?.timedOut === true || /timed out/iu.test(String(error?.message ?? ""));
  const aborted = error?.aborted === true || error?.name === "AbortError" || controller.signal.aborted;
  const current = await readStatus();
  const externallyCancelled = current?.status === "cancelled";
  finalStatus = {
    id: request.id,
    status: externallyCancelled || aborted ? "cancelled" : timedOut ? "timed_out" : "failed",
    createdAtUtc: request.createdAtUtc,
    queuedAtUtc,
    startedAtUtc: current?.startedAtUtc ?? null,
    completedAtUtc: new Date().toISOString(),
    runnerPid: process.pid,
    exitCode: error?.exitCode ?? null,
    readOnly: request.readOnly === true,
    queueWaitMs: lease?.queueWaitMs ?? null,
    executionSlot: lease?.slot ?? null,
    error: error instanceof Error ? error.message : String(error),
  };
  if (error?.stdout) stdout.write(String(error.stdout));
  if (error?.stderr) stderr.write(String(error.stderr));
} finally {
  clearInterval(cancellationPoll);
  await lease?.release().catch(() => {});
}

await new Promise((resolve) => stdout.end(resolve));
await new Promise((resolve) => stderr.end(resolve));
const current = await readStatus();
if (current?.status !== "cancelled" && finalStatus) await writeStatus(finalStatus);
