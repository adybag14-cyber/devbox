import path from "node:path";
import { fileURLToPath } from "node:url";
import { mkdir, open, readFile, rm, writeFile } from "node:fs/promises";
import { spawn } from "node:child_process";

import { config } from "./config.js";
import { prepareMcpImplementation } from "./mcp-implementation.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
export const projectRoot = path.resolve(__dirname, "..");

export const getLauncherPaths = (root = projectRoot) => {
  const runDir = path.join(root, "run");
  return {
    runDir,
    pidFile: path.join(runDir, "devbox.pid"),
    logFile: path.join(runDir, "devbox.log"),
    implementationFile: path.join(runDir, "devbox.implementation"),
    managedPidFile: path.join(runDir, "mcp.pid"),
    managedImplementationFile: path.join(runDir, "mcp.implementation"),
    managedStdoutLogFile: path.join(runDir, "mcp.stdout.log"),
    managedStderrLogFile: path.join(runDir, "mcp.stderr.log"),
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

const readImplementationFile = async (implementationFile) => {
  try {
    const value = String(await readFile(implementationFile, "utf8")).trim().toLowerCase();
    return ["rust", "js"].includes(value) ? value : null;
  } catch {
    return null;
  }
};

const ensureRunDir = async (runDir) => {
  await mkdir(runDir, { recursive: true });
};

export const probeServerHealth = async ({ url = buildServerUrl(), timeoutMs = 1000 } = {}) => {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), Math.max(100, timeoutMs));
  timer.unref?.();
  try {
    const response = await fetch(`${String(url).replace(/\/$/u, "")}/healthz`, { signal: controller.signal });
    return response.ok;
  } catch {
    return false;
  } finally {
    clearTimeout(timer);
  }
};

const writeGuardianDesiredState = async (paths, shouldRun, source) => {
  await writeFile(paths.guardianDesiredStateFile, `${JSON.stringify({
    ShouldRun: shouldRun,
    UpdatedAtUtc: new Date().toISOString(),
    Source: source,
  }, null, 2)}\n`, "utf8");
};

export const getServerStatus = async (root = projectRoot, { url = buildServerUrl(), healthTimeoutMs = 1000 } = {}) => {
  const paths = getLauncherPaths(root);
  await ensureRunDir(paths.runDir);

  const launcherPid = await readPidFile(paths.pidFile);
  const launcherAlive = isProcessAlive(launcherPid);
  if (!launcherAlive && launcherPid !== null) {
    await rm(paths.pidFile, { force: true });
    await rm(paths.implementationFile, { force: true });
  }
  if (launcherAlive) {
    const healthy = await probeServerHealth({ url, timeoutMs: healthTimeoutMs });
    return {
      running: true,
      healthy,
      pid: launcherPid,
      pidFile: paths.pidFile,
      logFile: paths.logFile,
      stderrLogFile: paths.logFile,
      manager: "portable-launcher",
      managedExternally: false,
      implementation: await readImplementationFile(paths.implementationFile),
      url,
    };
  }

  // Windows Guardian/PowerShell startup owns run/mcp.pid instead of devbox.pid.
  // Recognize that process for status/start de-duplication, but never assume
  // ownership of it for stop/restart operations.
  const managedPid = await readPidFile(paths.managedPidFile);
  const managedAlive = isProcessAlive(managedPid);
  if (managedAlive) {
    const healthy = await probeServerHealth({ url, timeoutMs: healthTimeoutMs });
    return {
      running: true,
      healthy,
      pid: managedPid,
      pidFile: paths.managedPidFile,
      logFile: paths.managedStdoutLogFile,
      stderrLogFile: paths.managedStderrLogFile,
      manager: "managed-mcp",
      managedExternally: true,
      implementation: await readImplementationFile(paths.managedImplementationFile),
      url,
    };
  }

  return {
    running: false,
    healthy: false,
    pid: null,
    pidFile: paths.pidFile,
    logFile: paths.logFile,
    stderrLogFile: paths.logFile,
    manager: null,
    managedExternally: false,
    implementation: null,
    url,
  };
};

export const startServerProcess = async (root = projectRoot, { preparedSpec = null } = {}) => {
  const paths = getLauncherPaths(root);
  await ensureRunDir(paths.runDir);
  const status = await getServerStatus(root);
  if (status.running) {
    await writeGuardianDesiredState(paths, true, "devbox start");
    if (status.healthy === false) {
      throw new Error(
        `A Devbox MCP process is already running as PID ${status.pid} (${status.manager}) but its health endpoint is not responding; refusing to start a competing server.`,
      );
    }
    return { ...status, started: false };
  }

  const spec = preparedSpec ?? await prepareMcpImplementation(root);
  await writeGuardianDesiredState(paths, true, "devbox start");
  const logHandle = await open(paths.logFile, "a");
  const child = spawn(spec.file, spec.args, {
    cwd: root,
    env: spec.env,
    detached: true,
    stdio: ["ignore", logHandle.fd, logHandle.fd],
  });

  child.unref();
  await writeFile(paths.pidFile, `${child.pid}\n`, "utf8");
  await writeFile(paths.implementationFile, `${spec.implementation}\n`, "utf8");
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
    await rm(paths.implementationFile, { force: true });
    const suffix = logTail ? `\nRecent ${paths.logFile}:\n${logTail}` : "";
    throw new Error(`${error instanceof Error ? error.message : String(error)}${suffix}`);
  }

  return {
    ...(await getServerStatus(root)),
    started: true,
    ready: true,
  };
};

export const stopServerProcess = async (root = projectRoot, statusOptions = {}) => {
  const paths = getLauncherPaths(root);
  const pid = await readPidFile(paths.pidFile);
  if (!isProcessAlive(pid)) {
    await rm(paths.pidFile, { force: true });
    await rm(paths.implementationFile, { force: true });
    const status = await getServerStatus(root, statusOptions);
    if (status.managedExternally) {
      return {
        ...status,
        stopped: false,
        stopRefused: true,
        note: `PID ${status.pid} is owned by the managed MCP lifecycle; use Guardian or the platform service/start-stop scripts rather than the portable launcher.`,
      };
    }
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
  await rm(paths.implementationFile, { force: true });
  const status = await getServerStatus(root);
  await writeGuardianDesiredState(paths, false, "devbox stop");
  return { ...status, stopped: true };
};

const runForegroundRustServer = async (spec, root) => new Promise((resolve, reject) => {
  const child = spawn(spec.file, spec.args, {
    cwd: root,
    env: spec.env,
    stdio: "inherit",
    windowsHide: false,
  });
  child.once("error", reject);
  child.once("exit", (code, signal) => {
    if (code === 0 || signal === "SIGINT" || signal === "SIGTERM") {
      resolve();
    } else {
      reject(new Error(`Rust MCP foreground process exited with code=${code ?? "null"}, signal=${signal ?? "null"}.`));
    }
  });
});

export const runLauncher = async (argv = process.argv.slice(2), root = projectRoot) => {
  const parsed = parseLauncherArgs(argv);

  if (parsed.command === "run") {
    const paths = getLauncherPaths(root);
    await ensureRunDir(paths.runDir);
    const spec = await prepareMcpImplementation(root);
    await writeGuardianDesiredState(paths, true, "devbox run");
    if (spec.implementation === "js") {
      await import("./server.js");
    } else {
      await runForegroundRustServer(spec, root);
    }
    return { command: "run", implementation: spec.implementation, url: buildServerUrl() };
  }

  if (parsed.command === "status") {
    return { command: "status", ...(await getServerStatus(root)) };
  }

  if (parsed.command === "stop") {
    return { command: "stop", ...(await stopServerProcess(root)) };
  }

  if (parsed.command === "restart") {
    const preparedSpec = await prepareMcpImplementation(root);
    const stopped = await stopServerProcess(root);
    if (stopped.stopRefused) {
      throw new Error(stopped.note);
    }
    return { command: "restart", ...(await startServerProcess(root, { preparedSpec })) };
  }

  return { command: "start", ...(await startServerProcess(root)) };
};
