import { createWriteStream } from "node:fs";
import { readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

import { execInDevbox, execReadOnlyInDevbox } from "../src/runtime.js";

const requestPath = process.argv[2];
if (!requestPath) throw new Error("job runner requires a request path");
const dir = path.dirname(requestPath);
const statusPath = path.join(dir, "status.json");
const stdoutPath = path.join(dir, "stdout.log");
const stderrPath = path.join(dir, "stderr.log");

const writeStatus = async (value) => {
  const temp = `${statusPath}.${process.pid}.tmp`;
  await writeFile(temp, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  await rename(temp, statusPath);
};

const request = JSON.parse(await readFile(requestPath, "utf8"));
const startedAtUtc = new Date().toISOString();
const base = {
  id: request.id,
  status: "running",
  createdAtUtc: request.createdAtUtc,
  startedAtUtc,
  completedAtUtc: null,
  runnerPid: process.pid,
  exitCode: null,
  readOnly: request.readOnly === true,
};
const initial = await readFile(statusPath, "utf8").then(JSON.parse).catch(() => null);
if (initial?.status === "cancelled") {
  process.exit(0);
}
await writeStatus(base);

const stdout = createWriteStream(stdoutPath, { flags: "a" });
const stderr = createWriteStream(stderrPath, { flags: "a" });
const controller = new AbortController();
for (const signal of ["SIGINT", "SIGTERM"]) {
  process.once(signal, () => controller.abort());
}

let finalStatus = { ...base };
try {
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
  const aborted = error?.aborted === true || controller.signal.aborted;
  const current = await readFile(statusPath, "utf8").then(JSON.parse).catch(() => null);
  const externallyCancelled = current?.status === "cancelled";
  finalStatus = {
    ...base,
    status: externallyCancelled || aborted ? "cancelled" : timedOut ? "timed_out" : "failed",
    completedAtUtc: new Date().toISOString(),
    exitCode: error?.exitCode ?? null,
    error: error instanceof Error ? error.message : String(error),
  };
  if (error?.stdout) stdout.write(String(error.stdout));
  if (error?.stderr) stderr.write(String(error.stderr));
}

await new Promise((resolve) => stdout.end(resolve));
await new Promise((resolve) => stderr.end(resolve));
const current = await readFile(statusPath, "utf8").then(JSON.parse).catch(() => null);
if (current?.status !== "cancelled") await writeStatus(finalStatus);
