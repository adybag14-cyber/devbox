import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { open, mkdir, readFile, rename, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const jobsRoot = path.join(projectRoot, "run", "jobs");
const runnerPath = path.join(projectRoot, "scripts", "devbox-job-runner.mjs");
const JOB_ID_RE = /^[a-z0-9][a-z0-9-]{7,80}$/iu;

const assertJobId = (jobId) => {
  const value = String(jobId ?? "").trim();
  if (!JOB_ID_RE.test(value)) throw new Error("Invalid Devbox job id.");
  return value;
};

const jobPaths = (jobId) => {
  const id = assertJobId(jobId);
  const dir = path.join(jobsRoot, id);
  return {
    id,
    dir,
    request: path.join(dir, "request.json"),
    status: path.join(dir, "status.json"),
    stdout: path.join(dir, "stdout.log"),
    stderr: path.join(dir, "stderr.log"),
  };
};

const writeJsonAtomic = async (filePath, value) => {
  const temp = `${filePath}.${process.pid}.${randomUUID()}.tmp`;
  await writeFile(temp, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  await rename(temp, filePath);
};

const readJson = async (filePath) => JSON.parse(await readFile(filePath, "utf8"));

const processAlive = (pid) => {
  if (!Number.isInteger(pid) || pid < 1) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return process.platform === "win32" && error?.code === "EPERM";
  }
};

const readTail = async (filePath, maxChars) => {
  try {
    const info = await stat(filePath);
    const bytes = Math.max(4096, Math.min(info.size, Math.max(1, maxChars) * 4));
    const handle = await open(filePath, "r");
    try {
      const buffer = Buffer.alloc(bytes);
      const { bytesRead } = await handle.read(buffer, 0, bytes, Math.max(0, info.size - bytes));
      const text = buffer.subarray(0, bytesRead).toString("utf8");
      return text.length > maxChars ? text.slice(-maxChars) : text;
    } finally {
      await handle.close();
    }
  } catch (error) {
    if (error?.code === "ENOENT") return "";
    throw error;
  }
};

export const startDevboxJob = async ({
  command,
  workingDir,
  timeoutSeconds = 7200,
  user,
  readOnly = false,
}) => {
  const id = `job-${Date.now().toString(36)}-${randomUUID().slice(0, 12)}`;
  const paths = jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const now = new Date().toISOString();
  const request = {
    id,
    command: String(command),
    workingDir: String(workingDir ?? ""),
    timeoutMs: Math.max(1000, Math.min(86400000, Number(timeoutSeconds) * 1000)),
    user: String(user ?? ""),
    readOnly: readOnly === true,
    createdAtUtc: now,
  };
  await Promise.all([
    writeJsonAtomic(paths.request, request),
    writeFile(paths.stdout, "", "utf8"),
    writeFile(paths.stderr, "", "utf8"),
    writeJsonAtomic(paths.status, {
      id,
      status: "queued",
      createdAtUtc: now,
      startedAtUtc: null,
      completedAtUtc: null,
      runnerPid: null,
      exitCode: null,
      readOnly: request.readOnly,
    }),
  ]);

  const child = spawn(process.execPath, [runnerPath, paths.request], {
    cwd: projectRoot,
    env: process.env,
    detached: true,
    stdio: "ignore",
    windowsHide: true,
  });
  child.on("error", () => {});
  child.unref();
  const runnerPid = child.pid ?? null;
  if (runnerPid) {
    const queued = await readJson(paths.status);
    if (queued.status === "queued") {
      await writeJsonAtomic(paths.status, { ...queued, runnerPid });
    }
  }

  return { id, status: "queued", runnerPid, jobDir: paths.dir };
};

export const getDevboxJobStatus = async (jobId) => {
  const paths = jobPaths(jobId);
  const value = await readJson(paths.status);
  const running = value.status === "running" && processAlive(Number(value.runnerPid));
  return {
    ...value,
    runnerAlive: running,
    jobDir: paths.dir,
  };
};

export const getDevboxJobLogs = async ({ jobId, maxChars = 20000 }) => {
  const paths = jobPaths(jobId);
  const bounded = Math.max(100, Math.min(100000, Number(maxChars) || 20000));
  const [stdout, stderr, statusValue] = await Promise.all([
    readTail(paths.stdout, bounded),
    readTail(paths.stderr, bounded),
    readJson(paths.status),
  ]);
  return { id: paths.id, status: statusValue.status, stdout, stderr, maxChars: bounded };
};

const killDetachedTree = async (pid) => {
  if (!Number.isInteger(pid) || pid < 1) return;
  if (process.platform === "win32") {
    await new Promise((resolve) => {
      const killer = spawn("taskkill.exe", ["/pid", String(pid), "/t", "/f"], {
        stdio: "ignore",
        windowsHide: true,
      });
      killer.once("error", resolve);
      killer.once("close", resolve);
    });
    return;
  }
  try { process.kill(-pid, "SIGTERM"); } catch {}
  await new Promise((resolve) => setTimeout(resolve, 500));
  try { process.kill(-pid, "SIGKILL"); } catch {}
};

export const cancelDevboxJob = async (jobId) => {
  const paths = jobPaths(jobId);
  const statusValue = await readJson(paths.status);
  if (["succeeded", "failed", "cancelled", "timed_out"].includes(statusValue.status)) return statusValue;
  const cancelled = {
    ...statusValue,
    status: "cancelled",
    completedAtUtc: new Date().toISOString(),
    cancelRequested: true,
  };
  await writeJsonAtomic(paths.status, cancelled);
  await killDetachedTree(Number(statusValue.runnerPid));
  return cancelled;
};

export const asyncJobsInternals = { assertJobId, jobPaths, readTail, processAlive };
