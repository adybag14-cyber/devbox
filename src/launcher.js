import path from "node:path";
import { fileURLToPath } from "node:url";
import { mkdir, open, readFile, rm, writeFile } from "node:fs/promises";
import { spawn } from "node:child_process";

import { config } from "./config.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
export const projectRoot = path.resolve(__dirname, "..");

export const getLauncherPaths = (root = projectRoot) => {
  const runDir = path.join(root, "run");
  return {
    runDir,
    pidFile: path.join(runDir, "devbox.pid"),
    logFile: path.join(runDir, "devbox.log"),
    guardianDesiredStateFile: path.join(runDir, "guardian.desired-state.json"),
  };
};

export const parseLauncherArgs = (argv = []) => {
  const first = String(argv[0] ?? "").trim().toLowerCase();
  const command = first || "start";

  if (["run", "serve", "foreground"].includes(command)) {
    return { command: "run", background: false };
  }

  if (["status", "stop", "restart", "start"].includes(command)) {
    return { command, background: command === "start" };
  }

  return { command: "start", background: true };
};

export const buildServerUrl = ({ host = config.host, port = config.port } = {}) => {
  const normalizedHost = ["0.0.0.0", "::", "[::]"].includes(String(host).trim()) ? "127.0.0.1" : String(host).trim() || "127.0.0.1";
  return `http://${normalizedHost}:${port}`;
};

export const waitForServerReady = async ({
  url = buildServerUrl(),
  pid = null,
  timeoutMs = 15000,
  pollIntervalMs = 100,
} = {}) => {
  const healthUrl = `${String(url).replace(/\/$/u, "")}/healthz`;
  const deadline = Date.now() + timeoutMs;
  let lastError = null;

  while (Date.now() < deadline) {
    if (pid !== null && !isProcessAlive(pid)) {
      throw new Error(`Devbox process ${pid} exited before ${healthUrl} became ready.`);
    }

    try {
      const response = await fetch(healthUrl);
      if (response.ok) {
        return { healthUrl, status: response.status };
      }
      lastError = new Error(`health endpoint returned HTTP ${response.status}`);
    } catch (error) {
      lastError = error;
    }

    await new Promise((resolve) => setTimeout(resolve, pollIntervalMs));
  }

  const detail = lastError instanceof Error && lastError.message ? ` Last error: ${lastError.message}` : "";
  throw new Error(`Timed out after ${timeoutMs} ms waiting for ${healthUrl}.${detail}`);
};

export const isProcessAlive = (pid) => {
  if (!Number.isInteger(pid) || pid <= 0) {
    return false;
  }

  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
};

const readPidFile = async (pidFile) => {
  try {
    const pidText = await readFile(pidFile, "utf8");
    const pid = Number.parseInt(String(pidText).trim(), 10);
    return Number.isInteger(pid) ? pid : null;
  } catch {
    return null;
  }
};

const ensureRunDir = async (runDir) => {
  await mkdir(runDir, { recursive: true });
};

const writeGuardianDesiredState = async (paths, shouldRun, source) => {
  await writeFile(paths.guardianDesiredStateFile, `${JSON.stringify({
    ShouldRun: shouldRun,
    UpdatedAtUtc: new Date().toISOString(),
    Source: source,
  }, null, 2)}\n`, "utf8");
};

export const getServerStatus = async (root = projectRoot) => {
  const paths = getLauncherPaths(root);
  await ensureRunDir(paths.runDir);
  const pid = await readPidFile(paths.pidFile);
  const running = isProcessAlive(pid);

  if (!running && pid !== null) {
    await rm(paths.pidFile, { force: true });
  }

  return {
    running,
    pid: running ? pid : null,
    pidFile: paths.pidFile,
    logFile: paths.logFile,
    url: buildServerUrl(),
  };
};

export const startServerProcess = async (root = projectRoot) => {
  const paths = getLauncherPaths(root);
  await ensureRunDir(paths.runDir);
  await writeGuardianDesiredState(paths, true, "devbox start");
  const status = await getServerStatus(root);
  if (status.running) {
    return { ...status, started: false };
  }

  const logHandle = await open(paths.logFile, "a");
  const child = spawn(process.execPath, [path.join(root, "src/server.js")], {
    cwd: root,
    env: process.env,
    detached: true,
    stdio: ["ignore", logHandle.fd, logHandle.fd],
  });

  child.unref();
  await writeFile(paths.pidFile, `${child.pid}\n`, "utf8");
  await logHandle.close();

  const spawnedStatus = await getServerStatus(root);
  try {
    await waitForServerReady({ url: spawnedStatus.url, pid: child.pid });
  } catch (error) {
    let logTail = "";
    try {
      const logText = await readFile(paths.logFile, "utf8");
      logTail = logText.slice(-8000).trim();
    } catch {
      // The log may not have been created yet.
    }
    if (isProcessAlive(child.pid)) {
      try {
        process.kill(child.pid, "SIGTERM");
      } catch {
        // Best-effort cleanup only.
      }
    }
    await rm(paths.pidFile, { force: true });
    const suffix = logTail ? `\nRecent ${paths.logFile}:\n${logTail}` : "";
    throw new Error(`${error instanceof Error ? error.message : String(error)}${suffix}`);
  }

  return {
    ...(await getServerStatus(root)),
    started: true,
    ready: true,
  };
};

export const stopServerProcess = async (root = projectRoot) => {
  const paths = getLauncherPaths(root);
  const pid = await readPidFile(paths.pidFile);
  if (!isProcessAlive(pid)) {
    await rm(paths.pidFile, { force: true });
    const status = await getServerStatus(root);
    await writeGuardianDesiredState(paths, false, "devbox stop");
    return { ...status, stopped: false };
  }

  process.kill(pid, "SIGTERM");
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (!isProcessAlive(pid)) {
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }

  if (isProcessAlive(pid)) {
    process.kill(pid, "SIGKILL");
  }

  await rm(paths.pidFile, { force: true });
  const status = await getServerStatus(root);
  await writeGuardianDesiredState(paths, false, "devbox stop");
  return { ...status, stopped: true };
};

export const runLauncher = async (argv = process.argv.slice(2), root = projectRoot) => {
  const parsed = parseLauncherArgs(argv);

  if (parsed.command === "run") {
    const paths = getLauncherPaths(root);
    await ensureRunDir(paths.runDir);
    await writeGuardianDesiredState(paths, true, "devbox run");
    await import("./server.js");
    return { command: "run", url: buildServerUrl() };
  }

  if (parsed.command === "status") {
    return { command: "status", ...(await getServerStatus(root)) };
  }

  if (parsed.command === "stop") {
    return { command: "stop", ...(await stopServerProcess(root)) };
  }

  if (parsed.command === "restart") {
    await stopServerProcess(root);
    return { command: "restart", ...(await startServerProcess(root)) };
  }

  return { command: "start", ...(await startServerProcess(root)) };
};
