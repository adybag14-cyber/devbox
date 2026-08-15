import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { open, mkdir, readFile, readdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { config } from "./config.js";
import { abortableSleep } from "./wait-utils.js";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const jobsRoot = process.env.MCP_JOBS_ROOT?.trim()
  ? path.resolve(process.env.MCP_JOBS_ROOT.trim())
  : path.join(projectRoot, "run", "jobs");
const runnerPath = path.join(projectRoot, "scripts", "devbox-job-runner.mjs");
const JOB_ID_RE = /^[a-z0-9][a-z0-9-]{7,80}$/iu;
const TERMINAL_STATUSES = new Set(["succeeded", "failed", "cancelled", "timed_out", "interrupted"]);
const RESOURCE_CLASSES = new Set(["auto", "watch", "light", "heavy", "io-heavy"]);

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
    cancel: path.join(dir, "cancel.requested"),
    heartbeat: path.join(dir, "heartbeat.json"),
  };
};

const writeJsonAtomic = async (filePath, value) => {
  const temp = `${filePath}.${process.pid}.${randomUUID()}.tmp`;
  await writeFile(temp, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  await rename(temp, filePath);
};

const readJson = async (filePath) => JSON.parse(await readFile(filePath, "utf8"));

const cancellationRequested = async (paths) => {
  try {
    await stat(paths.cancel);
    return true;
  } catch (error) {
    if (error?.code === "ENOENT") return false;
    throw error;
  }
};

const processAlive = (pid) => {
  if (!Number.isInteger(pid) || pid < 1) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return process.platform === "win32" && error?.code === "EPERM";
  }
};

const readHeartbeatState = async (paths) => {
  try {
    const [info, value] = await Promise.all([
      stat(paths.heartbeat),
      readJson(paths.heartbeat).catch(() => null),
    ]);
    return { value, ageMs: Math.max(0, Date.now() - info.mtimeMs) };
  } catch (error) {
    if (error?.code === "ENOENT") return { value: null, ageMs: null };
    throw error;
  }
};

const runnerOwnerState = async (paths, value) => {
  const pid = Number(value?.runnerPid);
  const processPresent = processAlive(pid);
  const heartbeat = await readHeartbeatState(paths);
  const normalizeInstance = (instance) => (typeof instance === "string" || typeof instance === "number") ? String(instance) : null;
  const expectedInstance = normalizeInstance(value?.runnerProcessInstance);
  const actualInstance = normalizeInstance(heartbeat.value?.runnerProcessInstance);
  const staleMs = Math.max(1000, config.mcpJobOrphanStaleMs);
  const identityMaxAgeMs = Math.max(60_000, staleMs * 4);
  const identityFresh = heartbeat.ageMs !== null && heartbeat.ageMs < identityMaxAgeMs;
  const identityComparable = identityFresh && expectedInstance !== null && actualInstance !== null;
  const instanceMatches = !identityComparable || actualInstance === expectedInstance;
  return {
    alive: processPresent && instanceMatches,
    processPresent,
    instanceMatches,
    heartbeat,
  };
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

const readTailAcrossRotations = async (filePath, maxChars, rotations = config.mcpJobLogRotations) => {
  let remaining = Math.max(1, maxChars);
  const chunks = [];
  for (let index = 0; index <= Math.max(0, rotations) && remaining > 0; index += 1) {
    const candidate = index === 0 ? filePath : `${filePath}.${index}`;
    const text = await readTail(candidate, remaining);
    if (!text) continue;
    chunks.unshift(text);
    remaining -= text.length;
  }
  const joined = chunks.join("");
  return joined.length > maxChars ? joined.slice(-maxChars) : joined;
};

const compactLegacyLogFile = async (filePath, maxBytes = config.mcpJobLogMaxBytes) => {
  try {
    const info = await stat(filePath);
    const limit = Math.max(4096, Number(maxBytes) || 32 * 1024 * 1024);
    if (info.size <= limit) return { compacted: false, bytes: info.size };
    const handle = await open(filePath, "r");
    try {
      const buffer = Buffer.alloc(limit);
      const { bytesRead } = await handle.read(buffer, 0, limit, Math.max(0, info.size - limit));
      await writeFile(filePath, buffer.subarray(0, bytesRead));
      return { compacted: true, previousBytes: info.size, bytes: bytesRead };
    } finally {
      await handle.close();
    }
  } catch (error) {
    if (error?.code === "ENOENT") return { compacted: false, bytes: 0 };
    throw error;
  }
};

const logMetadata = async (filePath, rotations = config.mcpJobLogRotations) => {
  const segments = [];
  let totalBytes = 0;
  for (let index = 0; index <= Math.max(0, rotations); index += 1) {
    const candidate = index === 0 ? filePath : `${filePath}.${index}`;
    try {
      const info = await stat(candidate);
      segments.push({ index, bytes: info.size });
      totalBytes += info.size;
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
    }
  }
  return { totalBytes, segments, rotated: segments.some((segment) => segment.index > 0) };
};

const normalizedProgramName = (program) => path.basename(String(program ?? "").replaceAll("\\", "/"))
  .toLowerCase()
  .replace(/\.(?:exe|cmd|bat|ps1)$/u, "");

const shellHasRipgrepSearch = (command) => {
  const tokens = String(command ?? "").match(/"[^"\r\n]*"|'[^'\r\n]*'|[^\s]+/gu) ?? [];
  for (let index = 0; index < tokens.length; index += 1) {
    const token = tokens[index].replace(/^["']|["']$/gu, "");
    if (normalizedProgramName(token) !== "rg") continue;
    const next = tokens[index + 1]?.replace(/^["']|["']$/gu, "");
    if (!next || next === "--version" || next === "-V") continue;
    return true;
  }
  return false;
};

const inferShellResourceClass = (command) => {
  const rawText = String(command ?? "");
  const text = rawText.toLowerCase();
  if (/playwright|selenium|gradlew?|mvnw?|ninja|cmake\s+--build|cargo\s+(?:build|test|bench|clippy)|rustc\s|zig\s+build|go\s+(?:build|test)|docker\s+(?:build|buildx\s+build)|pytest|npm\s+(?:(?:run\s+)?(?:build|test))|pnpm\s+(?:(?:run\s+)?(?:build|test))|yarn\s+(?:build|test)|bun\s+(?:build|test)|pip\s+wheel|python\s+-m\s+build|bazel|msbuild|dotnet\s+(?:build|test|publish)|(?:^|[;&|]\s*)make(?:\s|$)/u.test(text)) return "heavy";
  if (shellHasRipgrepSearch(rawText) || /(?:^|[\s;&|])find(?:\.exe)?\s|\bgrep\s+(?:(?:-[^\s]*[rR][^\s]*\s)|--recursive\b)|\bget-childitem\b[^\r\n;&|]*\s-recurse\b|(?:^|[\s;&|])du(?:\.exe)?(?:\s|$)|\b(?:robocopy|xcopy|rsync)\b|\bcp\s+-[^\s]*(?:r|R|a|A)[^\s]*\s|\brm\s+-[^\s]*(?:r|R)[^\s]*\s|\bremove-item\b[^\r\n;&|]*\s-recurse\b|\b(?:tar|7z|7zz)\s|\bzip\s+-[^\s]*r|\bgit\s+(?:clone|fetch|gc|repack)\b|\b(?:npm|pnpm|yarn|bun)\s+(?:ci|install|add)\b|\b(?:pip|pip3)\s+install\b|\b(?:python|python3|py)\s+-m\s+pip\s+install\b|\b(?:apt|apt-get|dnf|yum|pacman|apk|winget|choco|scoop)\s+(?:install|add|upgrade|update)\b/u.test(text)) return "io-heavy";
  if (/\bgh\s+run\s+watch\b|\bstart-sleep\b|\bsleep\s+\d+/u.test(text)) return "watch";
  return "light";
};

const inferProgramResourceClass = (program, rawArgs) => {
  const exe = normalizedProgramName(program);
  const raw = Array.isArray(rawArgs) ? rawArgs.map((value) => String(value).trim()) : [];
  const args = raw.map((value) => value.toLowerCase());
  const rawFirst = raw[0] ?? "";
  const first = args[0] ?? "";
  const second = args[1] ?? "";
  if (exe === "gh" && first === "run" && second === "watch") return "watch";
  if (["sleep", "start-sleep"].includes(exe)) return "watch";
  if (["wsl", "wsl.exe"].includes(exe)) return inferShellResourceClass(raw.join(" "));
  if (["find", "du", "robocopy", "xcopy", "rsync", "tar", "7z", "7zz", "zip", "unzip"].includes(exe)) return "io-heavy";
  if (exe === "rg" && args.length > 0 && first !== "--version" && rawFirst !== "-V") return "io-heavy";
  if (exe === "git" && ["clone", "fetch", "gc", "repack"].includes(first)) return "io-heavy";
  if (["npm", "pnpm", "yarn", "bun"].includes(exe) && ["ci", "install", "add"].includes(first)) return "io-heavy";
  if (["apt", "apt-get", "dnf", "yum", "pacman", "apk", "winget", "choco", "scoop"].includes(exe) && ["install", "add", "upgrade", "update", "-s"].includes(first)) return "io-heavy";
  if (["pip", "pip3"].includes(exe) && first === "install") return "io-heavy";
  if (["python", "python3", "py"].includes(exe) && first === "-m" && second === "pip" && args[2] === "install") return "io-heavy";
  if (["pwsh", "powershell"].includes(exe)) {
    const at = args.findIndex((arg) => arg === "-command" || arg === "-c");
    if (at >= 0 && args[at + 1]) return inferShellResourceClass(args[at + 1]);
  }
  const heavy =
    (exe === "cargo" && ["build", "test", "bench", "clippy"].includes(first)) ||
    ["rustc", "ninja", "make", "gmake", "mingw32-make", "bazel", "msbuild", "gradle", "gradlew", "mvn", "mvnw", "pytest", "py.test"].includes(exe) ||
    (exe === "cmake" && args.includes("--build")) ||
    (exe === "zig" && first === "build") ||
    (exe === "go" && ["build", "test", "install"].includes(first)) ||
    (exe === "docker" && (first === "build" || (first === "buildx" && second === "build") || (first === "compose" && args[2] === "build"))) ||
    (exe === "dotnet" && ["build", "test", "publish", "pack"].includes(first)) ||
    (["npm", "pnpm"].includes(exe) && (["build", "test"].includes(first) || (first === "run" && ["build", "test"].includes(second)))) ||
    (["yarn", "bun"].includes(exe) && (["build", "test"].includes(first) || (first === "run" && ["build", "test"].includes(second)))) ||
    (exe === "npx" && ["playwright", "selenium", "webpack", "vite", "tsc"].includes(first)) ||
    (["pip", "pip3"].includes(exe) && first === "wheel") ||
    (["python", "python3", "py"].includes(exe) && first === "-m" && (["pytest", "build"].includes(second) || (second === "pip" && args[2] === "wheel")));
  return heavy ? "heavy" : "light";
};

export const inferJobResourceClass = ({ command = "", program = "", args = [], requested = "auto" } = {}) => {
  const explicit = String(requested ?? "auto").trim().toLowerCase();
  if (RESOURCE_CLASSES.has(explicit) && explicit !== "auto") return explicit;
  return String(program ?? "").trim() ? inferProgramResourceClass(program, args) : inferShellResourceClass(command);
};

const startJobRequest = async (request) => {
  const id = `job-${Date.now().toString(36)}-${randomUUID().slice(0, 12)}`;
  const paths = jobPaths(id);
  await mkdir(paths.dir, { recursive: true });
  const now = new Date().toISOString();
  const normalized = {
    id,
    ...request,
    timeoutMs: Math.max(1000, Math.min(86400000, Number(request.timeoutMs) || 7200000)),
    resourceClass: inferJobResourceClass(request),
    runtimeMode: config.runtimeMode,
    createdAtUtc: now,
  };
  await Promise.all([
    writeJsonAtomic(paths.request, normalized),
    writeFile(paths.stdout, "", "utf8"),
    writeFile(paths.stderr, "", "utf8"),
    writeJsonAtomic(paths.status, {
      id,
      status: "queued",
      mode: normalized.mode,
      createdAtUtc: now,
      startedAtUtc: null,
      completedAtUtc: null,
      runnerPid: null,
      exitCode: null,
      readOnly: normalized.readOnly === true,
      resourceClass: normalized.resourceClass,
      runtimeMode: normalized.runtimeMode,
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
  return { id, status: "queued", runnerPid, jobDir: paths.dir, mode: normalized.mode, resourceClass: normalized.resourceClass };
};

export const startDevboxJob = async ({
  command,
  workingDir,
  timeoutSeconds = 7200,
  user,
  readOnly = false,
  resourceClass = "auto",
}) => startJobRequest({
  mode: "shell",
  command: String(command),
  workingDir: String(workingDir ?? ""),
  timeoutMs: Number(timeoutSeconds) * 1000,
  user: String(user ?? ""),
  readOnly: readOnly === true,
  requested: resourceClass,
});

export const startDevboxProgramJob = async ({
  program,
  args = [],
  input,
  workingDir,
  timeoutSeconds = 7200,
  user,
  resourceClass = "auto",
}) => startJobRequest({
  mode: "program",
  program: String(program),
  args: Array.isArray(args) ? args.map(String) : [],
  input: input === undefined ? undefined : String(input),
  workingDir: String(workingDir ?? ""),
  timeoutMs: Number(timeoutSeconds) * 1000,
  user: String(user ?? ""),
  readOnly: false,
  requested: resourceClass,
});

const reconcileStatus = async (paths, value) => {
  if (TERMINAL_STATUSES.has(value.status)) {
    return { ...value, runnerAlive: false, jobDir: paths.dir };
  }

  const cancelRequested = await cancellationRequested(paths);
  if (cancelRequested) {
    const runnerPid = Number(value.runnerPid);
    const owner = await runnerOwnerState(paths, value);
    const runnerAlive = owner.alive;
    const cancelled = {
      ...value,
      status: "cancelled",
      cancelRequested: true,
      completedAtUtc: value.completedAtUtc ?? new Date().toISOString(),
    };
    if (!runnerAlive) await writeJsonAtomic(paths.status, cancelled).catch(() => {});
    return { ...cancelled, runnerAlive, jobDir: paths.dir };
  }

  const runnerPid = Number(value.runnerPid);
  const owner = await runnerOwnerState(paths, value);
  const runnerAlive = owner.alive;
  const heartbeat = owner.heartbeat;
  const heartbeatAgeMs = heartbeat.ageMs;
  const referenceMs = Date.parse(value.startedAtUtc ?? value.queuedAtUtc ?? value.createdAtUtc ?? "");
  const statusAgeMs = Number.isFinite(referenceMs) ? Math.max(0, Date.now() - referenceMs) : Infinity;
  const staleMs = Math.max(1000, config.mcpJobOrphanStaleMs);
  const heartbeatStale = heartbeatAgeMs === null ? statusAgeMs >= staleMs : heartbeatAgeMs >= staleMs;

  if (!runnerAlive && heartbeatStale) {
    const childPid = Number(heartbeat.value?.childPid ?? value.childPid);
    const runtimeMode = String(value.runtimeMode ?? heartbeat.value?.runtimeMode ?? config.runtimeMode ?? "host");
    let orphanChildTerminated = false;
    let orphanDockerClientTerminated = false;
    let orphanChildCleanupSkipped = null;
    const childAppearsAlive = Number.isInteger(childPid) && childPid > 0 && processAlive(childPid);
    const childIdentityFreshEnough = heartbeatAgeMs !== null && heartbeatAgeMs <= 60000;
    if (childAppearsAlive && childIdentityFreshEnough && runtimeMode === "docker") {
      await killDetachedTree(childPid);
      orphanDockerClientTerminated = !processAlive(childPid);
      orphanChildCleanupSkipped = "docker-container-exec-not-force-killed-shared-container";
    } else if (childAppearsAlive && childIdentityFreshEnough) {
      await killDetachedTree(childPid);
      orphanChildTerminated = !processAlive(childPid);
    } else if (childAppearsAlive) {
      orphanChildCleanupSkipped = "heartbeat-too-old-to-safely-trust-reused-pid";
    }
    const interrupted = {
      ...value,
      status: "interrupted",
      completedAtUtc: value.completedAtUtc ?? new Date().toISOString(),
      runnerPid: Number.isInteger(runnerPid) && runnerPid > 0 ? runnerPid : value.runnerPid ?? null,
      interrupted: true,
      childPid: Number.isInteger(childPid) && childPid > 0 ? childPid : value.childPid ?? null,
      runtimeMode,
      orphanChildTerminated,
      orphanDockerClientTerminated,
      orphanChildCleanupSkipped,
      error: value.error || "Detached job runner disappeared before recording a terminal status.",
      heartbeatAgeMs,
    };
    await writeJsonAtomic(paths.status, interrupted).catch(() => {});
    return { ...interrupted, runnerAlive: false, jobDir: paths.dir };
  }

  return { ...value, runnerAlive, heartbeatAgeMs, jobDir: paths.dir };
};

export const getDevboxJobStatus = async (jobId) => {
  const paths = jobPaths(jobId);
  const value = await readJson(paths.status);
  return reconcileStatus(paths, value);
};

const shallowDirectoryBytes = async (dir) => {
  let total = 0;
  for (const entry of await readdir(dir, { withFileTypes: true }).catch(() => [])) {
    if (!entry.isFile()) continue;
    const metadata = await stat(path.join(dir, entry.name)).catch(() => null);
    if (metadata) total += metadata.size;
  }
  return total;
};

let maintenanceCycle = 0;

export const reconcileOrphanedDevboxJobs = async () => {
  const maintenanceStartedAt = Date.now();
  maintenanceCycle += 1;
  await mkdir(jobsRoot, { recursive: true });
  const names = await readdir(jobsRoot).catch(() => []);
  const summary = {
    scanned: 0,
    interrupted: 0,
    active: 0,
    terminal: 0,
    maintained: 0,
    compactedLogs: 0,
    deleted: 0,
    errors: 0,
    // The JS rollback performs one complete maintenance pass per invocation.
    // Progress fields therefore describe that single pass and are 100% at return.
    cycleCompleted: true,
    maintenanceCycle,
    maintenanceCursor: 0,
    maintenanceTotal: 0,
    maintenanceProgressPercent: 0,
    maintenanceCycleAgeMs: 0,
    storeBytes: 0,
    terminalRetained: 0,
    quotaDeleted: 0,
    quotaPressure: false,
    quotaCheckedAtUtc: new Date().toISOString(),
  };
  const terminalForQuota = [];
  const retentionHours = Math.max(0, Number(config.mcpJobRetentionHours) || 0);
  const retentionMs = retentionHours > 0 ? retentionHours * 60 * 60 * 1000 : 0;
  for (const name of names) {
    if (!JOB_ID_RE.test(name)) continue;
    summary.scanned += 1;
    const paths = jobPaths(name);
    try {
      const status = await getDevboxJobStatus(name);
      if (status.status === "interrupted") summary.interrupted += 1;
      const terminal = TERMINAL_STATUSES.has(status.status);
      if (!terminal) {
        summary.active += 1;
        summary.storeBytes += await shallowDirectoryBytes(paths.dir);
        continue;
      }
      summary.terminal += 1;

      const terminalAtMs = Date.parse(status.completedAtUtc ?? status.createdAtUtc ?? "");
      if (retentionMs > 0 && Number.isFinite(terminalAtMs) && Date.now() - terminalAtMs >= retentionMs) {
        await rm(paths.dir, { recursive: true, force: true });
        summary.deleted += 1;
        continue;
      }

      let rawStatus = await readJson(paths.status);
      let jobBytes = Number(rawStatus.maintenanceBytes);
      if (!Number.isFinite(jobBytes) || jobBytes < 0 || !rawStatus.maintenanceReconciledAtUtc) {
        let compacted = rawStatus.legacyLogsCompacted === true;
        if (!rawStatus.maintenanceReconciledAtUtc) {
          const [stdoutCompaction, stderrCompaction] = await Promise.all([
            compactLegacyLogFile(paths.stdout),
            compactLegacyLogFile(paths.stderr),
          ]);
          compacted = stdoutCompaction.compacted || stderrCompaction.compacted;
          if (compacted) summary.compactedLogs += 1;
          summary.maintained += 1;
        }
        jobBytes = await shallowDirectoryBytes(paths.dir);
        rawStatus = {
          ...rawStatus,
          maintenanceReconciledAtUtc: rawStatus.maintenanceReconciledAtUtc ?? new Date().toISOString(),
          legacyLogsCompacted: compacted,
          maintenanceBytes: jobBytes,
        };
        await writeJsonAtomic(paths.status, rawStatus);
      }
      summary.storeBytes += jobBytes;
      terminalForQuota.push({ name, dir: paths.dir, bytes: jobBytes, terminalAtMs: Number.isFinite(terminalAtMs) ? terminalAtMs : 0 });
    } catch {
      summary.errors += 1;
    }
  }
  terminalForQuota.sort((left, right) => left.terminalAtMs - right.terminalAtMs);
  summary.terminalRetained = terminalForQuota.length;
  const maxBytes = Math.max(0, Number(config.mcpJobStoreMaxBytes) || 0);
  const maxJobs = Math.max(0, Number(config.mcpJobStoreMaxTerminalJobs) || 0);
  const activeBytes = Math.max(0, summary.storeBytes - terminalForQuota.reduce((sum, job) => sum + job.bytes, 0));
  const bytePressure = maxBytes > 0 && summary.storeBytes > maxBytes;
  const countPressure = maxJobs > 0 && terminalForQuota.length > maxJobs;
  const canReduceBytePressure = bytePressure && activeBytes < maxBytes;
  const targetBytes = canReduceBytePressure ? Math.max(0, Math.floor(maxBytes * 0.9) - activeBytes) : Number.POSITIVE_INFINITY;
  const targetJobs = maxJobs > 0 ? Math.floor(maxJobs * 0.9) : Number.POSITIVE_INFINITY;
  let terminalBytes = terminalForQuota.reduce((sum, job) => sum + job.bytes, 0);
  summary.quotaPressure = bytePressure || countPressure;
  if (canReduceBytePressure || countPressure) {
    for (const job of terminalForQuota) {
      const bytesSatisfied = !canReduceBytePressure || terminalBytes <= targetBytes;
      const countSatisfied = summary.terminalRetained <= targetJobs;
      if (bytesSatisfied && countSatisfied) break;
      try {
        await rm(job.dir, { recursive: true, force: true });
      } catch {
        summary.errors += 1;
        continue;
      }
      terminalBytes = Math.max(0, terminalBytes - job.bytes);
      summary.storeBytes = Math.max(0, summary.storeBytes - job.bytes);
      summary.terminalRetained = Math.max(0, summary.terminalRetained - 1);
      summary.deleted += 1;
      summary.quotaDeleted += 1;
    }
  }
  summary.maintenanceCursor = summary.scanned;
  summary.maintenanceTotal = summary.scanned;
  summary.maintenanceProgressPercent = 100;
  summary.maintenanceCycleAgeMs = Math.max(0, Date.now() - maintenanceStartedAt);
  return summary;
};

export const waitForDevboxJobStatus = async (jobId, {
  waitSeconds = 0,
  terminalOnly = false,
  signal,
  pollMs = 250,
} = {}) => {
  const first = await getDevboxJobStatus(jobId);
  const boundedWaitMs = Math.max(0, Math.min(config.mcpWaitMaxSeconds * 1000, Number(waitSeconds) * 1000 || 0));
  if (boundedWaitMs <= 0 || TERMINAL_STATUSES.has(first.status)) return first;
  const initialStatus = first.status;
  const deadline = Date.now() + boundedWaitMs;
  let current = first;
  while (Date.now() < deadline) {
    await abortableSleep(Math.min(Math.max(50, pollMs), Math.max(1, deadline - Date.now())), signal);
    current = await getDevboxJobStatus(jobId);
    if (TERMINAL_STATUSES.has(current.status)) return current;
    if (!terminalOnly && current.status !== initialStatus) return current;
  }
  return { ...current, waitTimedOut: true, waitedMs: boundedWaitMs };
};

export const getDevboxJobLogs = async ({ jobId, maxChars = 20000 }) => {
  const paths = jobPaths(jobId);
  const bounded = Math.max(100, Math.min(100000, Number(maxChars) || 20000));
  const [stdout, stderr, statusValue, stdoutMeta, stderrMeta] = await Promise.all([
    readTailAcrossRotations(paths.stdout, bounded),
    readTailAcrossRotations(paths.stderr, bounded),
    getDevboxJobStatus(jobId),
    logMetadata(paths.stdout),
    logMetadata(paths.stderr),
  ]);
  return {
    id: paths.id,
    status: statusValue.status,
    stdout,
    stderr,
    maxChars: bounded,
    logs: {
      stdout: stdoutMeta,
      stderr: stderrMeta,
      maxBytesPerSegment: config.mcpJobLogMaxBytes,
      rotations: config.mcpJobLogRotations,
      truncated: stdoutMeta.rotated || stderrMeta.rotated,
    },
  };
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
  const statusValue = await getDevboxJobStatus(jobId);
  if (TERMINAL_STATUSES.has(statusValue.status) && !(statusValue.status === "cancelled" && statusValue.runnerAlive)) {
    return statusValue;
  }
  const cancelled = {
    ...statusValue,
    status: "cancelled",
    completedAtUtc: new Date().toISOString(),
    cancelRequested: true,
  };
  await writeFile(paths.cancel, `${cancelled.completedAtUtc}\n`, { encoding: "utf8", flag: "wx" }).catch((error) => {
    if (error?.code !== "EEXIST") throw error;
  });
  const ownership = await runnerOwnerState(paths, statusValue);
  if (ownership.alive) {
    await killDetachedTree(Number(statusValue.runnerPid));
  }
  return ownership.alive
    ? { ...cancelled, runnerAlive: false }
    : { ...cancelled, runnerAlive: false, cancellationKillSkipped: true };
};

export const asyncJobsInternals = {
  assertJobId,
  jobPaths,
  readTail,
  readTailAcrossRotations,
  processAlive,
  cancellationRequested,
  readHeartbeatState,
  runnerOwnerState,
  reconcileStatus,
  compactLegacyLogFile,
  TERMINAL_STATUSES,
};
