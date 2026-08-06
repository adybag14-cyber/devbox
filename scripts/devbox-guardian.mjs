#!/usr/bin/env node

import { existsSync } from "node:fs";
import { appendFile, mkdir, open, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { execFile, spawn } from "node:child_process";
import { promisify } from "node:util";
import { fileURLToPath, pathToFileURL } from "node:url";

import {
  classifyReadiness,
  isRepairAllowed,
  normalizeRuntimeMode,
  resolveSelectedRuntime,
  selectRepairScope,
  updateDockerRepairPolicy,
} from "../src/guardian-core.js";
import { parseEnvText } from "../src/env.js";

const execFileAsync = promisify(execFile);

export const resolveWindowsPowerShellExecutable = (environment = process.env) => {
  const legacy = path.join(environment.SystemRoot || "C:\\Windows", "System32", "WindowsPowerShell", "v1.0", "powershell.exe");
  const pwsh7 = path.join(environment.ProgramFiles || "C:\\Program Files", "PowerShell", "7", "pwsh.exe");
  const configured = String(environment.POWERSHELL_EXE ?? "").trim();
  const candidates = [configured, pwsh7, legacy].filter(Boolean);
  for (const candidate of candidates) {
    if (!path.isAbsolute(candidate) || existsSync(candidate)) {
      return candidate;
    }
  }
  return "powershell.exe";
};

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const defaultProjectRoot = path.resolve(scriptDir, "..");

const parseArgs = (argv) => {
  const options = {
    projectRoot: process.env.DEVBOX_PROJECT_ROOT || defaultProjectRoot,
    pollSeconds: 10,
    failureThreshold: 3,
    repairCooldownSeconds: 120,
    dockerProbeTimeoutSeconds: 5,
    dockerBackoffBaseSeconds: 60,
    dockerBackoffMaxSeconds: 1800,
    dockerCircuitFailureThreshold: 3,
    dockerCircuitOpenSeconds: 3600,
    once: false,
    noRepair: false,
  };

  const numeric = new Map([
    ["--poll-seconds", "pollSeconds"],
    ["--failure-threshold", "failureThreshold"],
    ["--repair-cooldown-seconds", "repairCooldownSeconds"],
    ["--docker-probe-timeout-seconds", "dockerProbeTimeoutSeconds"],
    ["--docker-backoff-base-seconds", "dockerBackoffBaseSeconds"],
    ["--docker-backoff-max-seconds", "dockerBackoffMaxSeconds"],
    ["--docker-circuit-failure-threshold", "dockerCircuitFailureThreshold"],
    ["--docker-circuit-open-seconds", "dockerCircuitOpenSeconds"],
  ]);

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--project-root") {
      options.projectRoot = argv[++index];
    } else if (numeric.has(argument)) {
      const parsed = Number.parseInt(argv[++index] ?? "", 10);
      if (!Number.isInteger(parsed) || parsed < 1) {
        throw new Error(`${argument} requires a positive integer`);
      }
      options[numeric.get(argument)] = parsed;
    } else if (argument === "--once") {
      options.once = true;
    } else if (argument === "--no-repair") {
      options.noRepair = true;
    } else {
      throw new Error(`Unknown guardian option: ${argument}`);
    }
  }

  options.projectRoot = path.resolve(options.projectRoot);
  return options;
};

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

export const classifyCommandFailure = (
  error,
  { startedAtMs = Date.now(), nowMs = Date.now(), fallbackExitCode = 1, timeoutMs = null } = {},
) => {
  const elapsedMs = Math.max(0, nowMs - startedAtMs);
  const timedOut = Boolean(
    error?.killed ||
    error?.timedOut ||
    error?.code === "ETIMEDOUT" ||
    error?.code === "ERR_CHILD_PROCESS_TIMEOUT" ||
    (Number.isFinite(timeoutMs) && elapsedMs >= timeoutMs),
  );
  const numericExitCode = Number.isInteger(error?.code) ? error.code : fallbackExitCode;

  return {
    exitCode: timedOut ? 124 : numericExitCode,
    timedOut,
    signal: error?.signal ? String(error.signal) : null,
    elapsedMs,
  };
};

const createChildProcessError = (message, details = {}) =>
  Object.assign(new Error(message), {
    code: details.code ?? null,
    killed: details.killed === true,
    timedOut: details.timedOut === true,
    signal: details.signal ?? null,
    stdout: details.stdout ?? "",
    stderr: details.stderr ?? "",
  });

export const runProcessUntilExit = (file, args, options = {}) =>
  new Promise((resolve, reject) => {
    const startedAtMs = Date.now();
    const timeoutMs = Math.max(1, options.timeout ?? 150000);
    const maxBuffer = Math.max(1, options.maxBuffer ?? 4 * 1024 * 1024);
    let stdout = "";
    let stderr = "";
    let stdoutBytes = 0;
    let stderrBytes = 0;
    let timedOut = false;
    let bufferExceeded = false;
    let settled = false;
    let timeoutTimer;
    let forcedSettleTimer;

    const child = spawn(file, args, {
      cwd: options.cwd,
      env: options.env ?? process.env,
      windowsHide: options.windowsHide !== false,
      stdio: ["pipe", "pipe", "pipe"],
    });

    const cleanup = () => {
      clearTimeout(timeoutTimer);
      clearTimeout(forcedSettleTimer);
      child.stdout?.destroy();
      child.stderr?.destroy();
      child.stdin?.destroy();
    };
    const settle = (handler, value) => {
      if (settled) return;
      settled = true;
      cleanup();
      handler(value);
    };
    const snapshotError = (message, details = {}) =>
      createChildProcessError(message, {
        ...details,
        stdout,
        stderr,
      });
    const appendChunk = (stream, chunk) => {
      const bytes = Buffer.byteLength(chunk);
      if (stream === "stdout") {
        stdoutBytes += bytes;
        if (stdoutBytes <= maxBuffer) stdout += chunk;
      } else {
        stderrBytes += bytes;
        if (stderrBytes <= maxBuffer) stderr += chunk;
      }
      if (!bufferExceeded && (stdoutBytes > maxBuffer || stderrBytes > maxBuffer)) {
        bufferExceeded = true;
        child.kill();
        clearTimeout(timeoutTimer);
        forcedSettleTimer = setTimeout(() => {
          settle(reject, snapshotError(`Command output exceeded ${maxBuffer} bytes.`, {
            code: "ENOBUFS",
            killed: true,
          }));
        }, 3000);
        forcedSettleTimer.unref?.();
      }
    };

    child.stdout.setEncoding(options.encoding ?? "utf8");
    child.stderr.setEncoding(options.encoding ?? "utf8");
    child.stdout.on("data", (chunk) => appendChunk("stdout", chunk));
    child.stderr.on("data", (chunk) => appendChunk("stderr", chunk));
    child.once("error", (error) => {
      settle(reject, snapshotError(error.message, { code: error.code }));
    });
    child.once("exit", (code, signal) => {
      setTimeout(() => {
        if (bufferExceeded) {
          settle(reject, snapshotError(`Command output exceeded ${maxBuffer} bytes.`, { code: "ENOBUFS", killed: true, signal }));
        } else if (timedOut) {
          settle(reject, snapshotError(`Command timed out after ${timeoutMs} ms.`, {
            code,
            killed: true,
            timedOut: true,
            signal,
          }));
        } else if (code !== 0) {
          settle(reject, snapshotError(stderr.trim() || stdout.trim() || `${file} exited with code ${code}.`, {
            code,
            signal,
          }));
        } else {
          settle(resolve, { exitCode: 0, stdout, stderr, elapsedMs: Date.now() - startedAtMs });
        }
      }, 25);
    });

    timeoutTimer = setTimeout(() => {
      timedOut = true;
      child.kill();
      forcedSettleTimer = setTimeout(() => {
        settle(reject, snapshotError(`Command timed out after ${timeoutMs} ms.`, {
          code: 124,
          killed: true,
          timedOut: true,
        }));
      }, 3000);
      forcedSettleTimer.unref?.();
    }, timeoutMs);
    timeoutTimer.unref?.();

    if (options.input !== undefined) {
      child.stdin.end(options.input);
    } else {
      child.stdin.end();
    }
  });

const psSingleQuote = (value) => String(value).replace(/'/gu, "''");

export const buildWindowsGuardianRepairArgs = ({
  scriptPath,
  selectedRuntime,
  settings = {},
  repairScope = "full",
}) => {
  const invocation = [
    `& '${psSingleQuote(scriptPath)}'`,
    `-Runtime '${psSingleQuote(selectedRuntime)}'`,
    settings.Public ? "-Public" : "",
    settings.OAuth ? "-OAuth" : "",
    repairScope === "public-tunnel" ? "-TunnelOnly" : "",
  ].filter(Boolean).join(" ");
  const command = `
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
$exitCode = 0
try {
  ${invocation}
  if (-not $?) {
    $exitCode = 1
  }
} catch {
  [Console]::Error.WriteLine(($_ | Out-String))
  $exitCode = 1
} finally {
  [Console]::Out.Flush()
  [Console]::Error.Flush()
  [System.Environment]::Exit($exitCode)
}
`;

  return [
    "-NoLogo",
    "-NoProfile",
    "-NonInteractive",
    "-ExecutionPolicy",
    "Bypass",
    "-EncodedCommand",
    Buffer.from(command, "utf16le").toString("base64"),
  ];
};

const readJson = async (filePath, fallback = null) => {
  try {
    return JSON.parse(await readFile(filePath, "utf8"));
  } catch {
    return fallback;
  }
};

const writeJsonAtomic = async (filePath, value) => {
  await mkdir(path.dirname(filePath), { recursive: true });
  const temporaryPath = `${filePath}.${process.pid}.${Date.now()}.tmp`;
  await writeFile(temporaryPath, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  try {
    await rename(temporaryPath, filePath);
  } catch (error) {
    if (process.platform !== "win32") {
      throw error;
    }
    await rm(filePath, { force: true });
    await rename(temporaryPath, filePath);
  }
};

const loadGuardianEnv = async (projectRoot) => {
  const values = {};
  for (const name of [".env", ".env.runtime"]) {
    try {
      Object.assign(values, parseEnvText(await readFile(path.join(projectRoot, name), "utf8")));
    } catch {
      // Missing env files are valid during first-run setup.
    }
  }
  return { ...values, ...process.env };
};

const normalizePublicUrl = (value) => {
  const raw = String(value ?? "").trim().replace(/\/+$/u, "");
  if (!raw) {
    return "";
  }
  return /^https?:\/\//iu.test(raw) ? raw : `https://${raw}`;
};

export const isProcessAlive = (pid, signalProcess = process.kill.bind(process)) => {
  if (!Number.isInteger(pid) || pid < 1) {
    return false;
  }
  try {
    signalProcess(pid, 0);
    return true;
  } catch (error) {
    // Windows reports EPERM for a live process in another integrity/session boundary.
    return error?.code === "EPERM";
  }
};

export const isGuardianCommandLine = (commandLine, projectRoot) => {
  const normalizedCommandLine = String(commandLine ?? "").replaceAll("\\", "/").toLowerCase();
  const expectedScript = path.join(projectRoot, "scripts", "devbox-guardian.mjs").replaceAll("\\", "/").toLowerCase();
  return normalizedCommandLine.includes(expectedScript);
};

const readProcessCommandLine = async (pid) => {
  try {
    if (process.platform === "win32") {
      const powerShell = resolveWindowsPowerShellExecutable(process.env);
      const command = [
        `$process = Get-CimInstance Win32_Process -Filter 'ProcessId=${pid}' -ErrorAction Stop`,
        "[Console]::Out.Write([string]$process.CommandLine)",
      ].join("; ");
      const result = await execFileAsync(powerShell, [
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy", "Bypass",
        "-Command", command,
      ], { encoding: "utf8", timeout: 5000, windowsHide: true });
      return String(result.stdout ?? "");
    }

    if (process.platform === "linux") {
      return (await readFile(`/proc/${pid}/cmdline`, "utf8")).replaceAll("\0", " ");
    }

    const result = await execFileAsync("ps", ["-p", String(pid), "-o", "command="], {
      encoding: "utf8",
      timeout: 5000,
    });
    return String(result.stdout ?? "");
  } catch {
    return null;
  }
};

export const isGuardianLockOwner = async (
  pid,
  projectRoot,
  {
    processAlive = isProcessAlive,
    commandLineReader = readProcessCommandLine,
    heartbeat = null,
    lockModifiedAtMs = 0,
    nowMs = Date.now(),
  } = {},
) => {
  if (!processAlive(pid)) {
    return false;
  }
  const commandLine = await commandLineReader(pid);
  if (String(commandLine ?? "").trim()) {
    return isGuardianCommandLine(commandLine, projectRoot);
  }

  // Scheduled-task process command lines can be hidden even from the owning user.
  // Corroborate a blank result with a recent heartbeat or a short startup grace period.
  const observedAtMs = Date.parse(String(heartbeat?.ObservedAtUtc ?? ""));
  const heartbeatMatches = Number(heartbeat?.SupervisorPid) === pid
    && Number.isFinite(observedAtMs)
    && nowMs - observedAtMs >= 0
    && nowMs - observedAtMs < 60000;
  const lockIsNew = Number.isFinite(lockModifiedAtMs)
    && lockModifiedAtMs > 0
    && nowMs - lockModifiedAtMs >= 0
    && nowMs - lockModifiedAtMs < 30000;
  return heartbeatMatches || lockIsNew;
};

const readPid = async (filePath) => {
  try {
    const pid = Number.parseInt((await readFile(filePath, "utf8")).trim(), 10);
    return Number.isInteger(pid) && pid > 0 ? pid : null;
  } catch {
    return null;
  }
};

const findMcpProcess = async (runDir) => {
  for (const name of ["mcp.pid", "devbox.pid"]) {
    const pidFile = path.join(runDir, name);
    const pid = await readPid(pidFile);
    if (isProcessAlive(pid)) {
      return { pid, pidFile };
    }
  }
  return { pid: null, pidFile: null };
};

/**
 * True when the target Windows process has an elevated admin token.
 * Non-Windows platforms always report elevated (no UAC host bridge).
 */
export const isWindowsProcessElevated = async (pid) => {
  if (process.platform !== "win32") {
    return true;
  }
  if (!Number.isInteger(pid) || pid < 1) {
    return false;
  }

  const powerShell = resolveWindowsPowerShellExecutable(process.env);
  // Fresh process each probe; still guard Add-Type redefinition and ignore CLIXML noise.
  const command = `
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$pidToCheck = ${pid}
if (-not ('DevboxTokenElev' -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class DevboxTokenElev {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint a, bool b, int c);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool GetTokenInformation(IntPtr t, int c, IntPtr i, int l, out int r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
}
$hProc = [DevboxTokenElev]::OpenProcess(0x1000, $false, $pidToCheck)
if ($hProc -eq [IntPtr]::Zero) { [Console]::Out.Write('{"elevated":false}'); exit 0 }
$hTok = [IntPtr]::Zero
if (-not [DevboxTokenElev]::OpenProcessToken($hProc, 0x0008, [ref]$hTok)) {
  [void][DevboxTokenElev]::CloseHandle($hProc)
  [Console]::Out.Write('{"elevated":false}')
  exit 0
}
$len = 0
[void][DevboxTokenElev]::GetTokenInformation($hTok, 20, [IntPtr]::Zero, 0, [ref]$len)
$ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($len)
$ok = [DevboxTokenElev]::GetTokenInformation($hTok, 20, $ptr, $len, [ref]$len)
$elevated = $false
if ($ok) { $elevated = [Runtime.InteropServices.Marshal]::ReadInt32($ptr) -ne 0 }
[Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
[void][DevboxTokenElev]::CloseHandle($hTok)
[void][DevboxTokenElev]::CloseHandle($hProc)
[Console]::Out.Write((@{ elevated = [bool]$elevated } | ConvertTo-Json -Compress))
`;

  try {
    const result = await execFileAsync(powerShell, [
      "-NoProfile",
      "-NonInteractive",
      "-ExecutionPolicy",
      "Bypass",
      "-Command",
      command,
    ], { encoding: "utf8", timeout: 8000, windowsHide: true });
    const stdout = String(result.stdout ?? "");
    const match = stdout.match(/\{"elevated":(true|false)\}/u);
    if (match) {
      return match[1] === "true";
    }
    const parsed = JSON.parse(stdout.trim() || "{}");
    return parsed.elevated === true;
  } catch {
    return false;
  }
};

const testHealth = async (url, timeoutSeconds = 5) => {
  if (!url) {
    return null;
  }
  try {
    const response = await fetch(url, { signal: AbortSignal.timeout(timeoutSeconds * 1000) });
    const body = await response.text();
    return response.ok && /ok/iu.test(body);
  } catch {
    return false;
  }
};

const resolveDockerExecutable = (environment) => environment.DOCKER_EXE?.trim() || "docker";

const runDocker = async (environment, args, timeoutSeconds) => {
  const startedAtMs = Date.now();
  try {
    const result = await execFileAsync(resolveDockerExecutable(environment), args, {
      cwd: environment.DEVBOX_PROJECT_ROOT,
      encoding: "utf8",
      timeout: timeoutSeconds * 1000,
      windowsHide: true,
      maxBuffer: 2 * 1024 * 1024,
    });
    return { exitCode: 0, stdout: result.stdout ?? "", stderr: result.stderr ?? "" };
  } catch (error) {
    const failure = classifyCommandFailure(error, { startedAtMs, fallbackExitCode: 127 });
    return {
      exitCode: failure.exitCode,
      timedOut: failure.timedOut,
      signal: failure.signal,
      elapsedMs: failure.elapsedMs,
      stdout: String(error.stdout ?? ""),
      stderr: String(error.stderr ?? error.message ?? ""),
    };
  }
};

export const inspectContainer = async (environment, containerName, timeoutSeconds, dockerRunner = runDocker) => {
  const result = await dockerRunner(environment, [
    "container",
    "inspect",
    containerName,
    "--format",
    "{{.State.Running}}",
  ], timeoutSeconds);
  if (result.exitCode === 0) {
    return { exists: true, running: result.stdout.trim() === "true", result };
  }
  const text = `${result.stdout}\n${result.stderr}`;
  if (/no such (object|container)/iu.test(text)) {
    return { exists: false, running: false, result };
  }
  return { exists: null, running: false, result };
};

export const ensureDockerContainer = async (environment, settings, timeoutSeconds, dockerRunner = runDocker) => {
  const containerName = settings.DevboxContainerName || environment.DEVBOX_CONTAINER_NAME || "chatgpt-devbox-runtime";
  const imageName = environment.DEVBOX_IMAGE_NAME || "chatgpt-devbox-runtime:local";
  const hostWorkspace = environment.HOST_WORKSPACE_PATH || path.join(environment.DEVBOX_PROJECT_ROOT, "workspace");
  const containerWorkspace = environment.DEVBOX_WORKSPACE_PATH || "/workspace";
  let inspection = await inspectContainer(environment, containerName, timeoutSeconds, dockerRunner);

  if (inspection.exists === null) {
    throw new Error(`Docker inspect failed; refusing a conflicting docker run: ${inspection.result.stderr.trim()}`);
  }
  if (inspection.running) {
    return { action: "already-running", containerName };
  }
  if (inspection.exists) {
    const started = await dockerRunner(environment, ["start", containerName], Math.max(timeoutSeconds, 30));
    if (started.exitCode === 0) {
      inspection = await inspectContainer(environment, containerName, timeoutSeconds, dockerRunner);
      if (inspection.running) {
        return { action: "started-existing", containerName };
      }
      if (inspection.exists === null) {
        throw new Error(`Docker inspect failed after starting ${containerName}: ${inspection.result.stderr.trim()}`);
      }
    }

    const removed = await dockerRunner(environment, ["rm", "-f", containerName], Math.max(timeoutSeconds, 30));
    if (removed.exitCode !== 0) {
      throw new Error(`Could not replace stale container ${containerName}: ${removed.stderr.trim()}`);
    }
  }

  const createArgs = [
    "run", "-d", "--name", containerName, "--init", "-w", containerWorkspace,
    "-v", `${hostWorkspace}:${containerWorkspace}`, imageName, "sleep", "infinity",
  ];
  const created = await dockerRunner(environment, createArgs, Math.max(timeoutSeconds, 60));
  if (created.exitCode !== 0 && /conflict.*container name/isu.test(`${created.stdout}\n${created.stderr}`)) {
    inspection = await inspectContainer(environment, containerName, timeoutSeconds, dockerRunner);
    if (inspection.exists) {
      const started = await dockerRunner(environment, ["start", containerName], Math.max(timeoutSeconds, 30));
      if (started.exitCode === 0) {
        return { action: "started-raced-existing", containerName };
      }
    }
  }
  if (created.exitCode !== 0) {
    throw new Error(`Could not create container ${containerName}: ${created.stderr.trim()}`);
  }
  return { action: "created", containerName };
};

export const runRepairCommand = async ({
  environment,
  settings,
  selectedRuntime,
  repairScope = "full",
  timeoutSeconds,
}) => {
  if (selectedRuntime === "docker" && process.platform !== "win32") {
    await ensureDockerContainer(environment, settings, Math.min(timeoutSeconds, 30));
  }

  const override = environment.DEVBOX_GUARDIAN_REPAIR_COMMAND?.trim();
  if (override) {
    const shell = process.platform === "win32" ? (environment.ComSpec || "cmd.exe") : (environment.SHELL || "/bin/sh");
    const args = process.platform === "win32" ? ["/d", "/s", "/c", override] : ["-lc", override];
    return runProcessUntilExit(shell, args, {
      cwd: environment.DEVBOX_PROJECT_ROOT,
      env: { ...environment, DEVBOX_GUARDIAN_REPAIR_SCOPE: repairScope },
      encoding: "utf8",
      timeout: timeoutSeconds * 1000,
      windowsHide: true,
      maxBuffer: 4 * 1024 * 1024,
    });
  }

  if (process.platform === "win32") {
    const powerShell = resolveWindowsPowerShellExecutable(environment);
    const args = buildWindowsGuardianRepairArgs({
      scriptPath: path.join(environment.DEVBOX_PROJECT_ROOT, "scripts", "Start-ChatGptDevboxMcp.ps1"),
      selectedRuntime,
      settings,
      repairScope,
    });
    return runProcessUntilExit(powerShell, args, {
      cwd: environment.DEVBOX_PROJECT_ROOT,
      encoding: "utf8",
      timeout: timeoutSeconds * 1000,
      windowsHide: true,
      maxBuffer: 4 * 1024 * 1024,
    });
  }

  return runProcessUntilExit(process.execPath, [path.join(environment.DEVBOX_PROJECT_ROOT, "bin", "devbox.js"), "restart"], {
    cwd: environment.DEVBOX_PROJECT_ROOT,
    env: { ...environment, DEVBOX_RUNTIME_MODE: selectedRuntime },
    encoding: "utf8",
    timeout: timeoutSeconds * 1000,
    maxBuffer: 4 * 1024 * 1024,
  });
};

const createPaths = (projectRoot) => {
  const runDir = path.join(projectRoot, "run");
  const guardianDir = path.join(runDir, "guardian");
  return {
    runDir,
    guardianDir,
    repairsDir: path.join(guardianDir, "repairs"),
    settings: path.join(runDir, "guardian.settings.json"),
    desired: path.join(runDir, "guardian.desired-state.json"),
    heartbeat: path.join(guardianDir, "heartbeat.json"),
    state: path.join(guardianDir, "state.json"),
    pid: path.join(guardianDir, "guardian.pid"),
    lock: path.join(guardianDir, "guardian.lock"),
    log: path.join(guardianDir, "guardian.log"),
    repairPolicy: path.join(guardianDir, "repair-policy.json"),
    lastRepair: path.join(guardianDir, "last-repair.json"),
  };
};

export const acquireGuardianLock = async (lockPath, projectRoot, ownerChecker = isGuardianLockOwner) => {
  await mkdir(path.dirname(lockPath), { recursive: true });
  try {
    const handle = await open(lockPath, "wx");
    await handle.writeFile(`${process.pid}\n`, "utf8");
    await handle.close();
    return true;
  } catch (error) {
    if (error.code !== "EEXIST") {
      throw error;
    }
    const existingPid = await readPid(lockPath);
    const [heartbeat, lockInfo] = await Promise.all([
      readJson(path.join(path.dirname(lockPath), "heartbeat.json"), null),
      stat(lockPath).catch(() => null),
    ]);
    if (await ownerChecker(existingPid, projectRoot, {
      heartbeat,
      lockModifiedAtMs: lockInfo?.mtimeMs ?? 0,
    })) {
      return false;
    }
    await rm(lockPath, { force: true });
    return acquireGuardianLock(lockPath, projectRoot, ownerChecker);
  }
};

const main = async () => {
  const options = parseArgs(process.argv.slice(2));
  const paths = createPaths(options.projectRoot);
  await mkdir(paths.repairsDir, { recursive: true });
  if (!(await acquireGuardianLock(paths.lock, options.projectRoot))) {
    return;
  }

  const ownerPid = Number.parseInt(process.env.DEVBOX_GUARDIAN_WRAPPER_PID ?? "", 10);
  const guardianPid = Number.isInteger(ownerPid) && ownerPid > 0 ? ownerPid : process.pid;
  let stopping = false;
  let unhealthyCount = 0;
  let lastRepairAtMs = 0;
  let repairPolicy = await readJson(paths.repairPolicy, {});

  const log = async (level, message) => {
    await appendFile(paths.log, `${new Date().toISOString()} [${level}] ${message}\n`, "utf8");
  };

  const probe = async () => {
    const [environment, settingsValue, desiredValue] = await Promise.all([
      loadGuardianEnv(options.projectRoot),
      readJson(paths.settings, {}),
      readJson(paths.desired, { ShouldRun: true, Source: "devbox-guardian.mjs" }),
    ]);
    environment.DEVBOX_PROJECT_ROOT = options.projectRoot;
    const settings = settingsValue ?? {};
    const desired = desiredValue ?? { ShouldRun: true };
    const shouldRun = desired.ShouldRun !== false;
    const port = Number.parseInt(settings.Port ?? environment.PORT ?? "8100", 10) || 8100;
    const publicBaseUrl = normalizePublicUrl(settings.PublicBaseUrl || environment.PUBLIC_BASE_URL || environment.CLOUDFLARED_PUBLIC_HOSTNAME);
    const publicEnabled = settings.Public === true || Boolean(publicBaseUrl);
    const mcpProcess = await findMcpProcess(paths.runDir);
    const [localHealth, publicHealth] = await Promise.all([
      testHealth(`http://127.0.0.1:${port}/healthz`),
      publicEnabled ? testHealth(`${publicBaseUrl}/healthz`) : Promise.resolve(null),
    ]);
    const runtimeMode = normalizeRuntimeMode(settings.RuntimeMode || environment.DEVBOX_RUNTIME_MODE || "auto");
    const selectedRuntime = resolveSelectedRuntime({
      runtimeMode,
      selectedRuntime: settings.SelectedRuntime,
      platform: process.platform,
      legacyHostHealthy: Boolean(mcpProcess.pid && localHealth),
    });

    let dockerReady = null;
    let devboxContainerRunning = null;
    let dockerProbe = null;
    if (selectedRuntime === "docker" && shouldRun) {
      dockerProbe = await runDocker(environment, ["version", "--format", "{{.Server.Version}}"], options.dockerProbeTimeoutSeconds);
      dockerReady = dockerProbe.exitCode === 0;
      if (dockerReady) {
        const inspection = await inspectContainer(
          environment,
          settings.DevboxContainerName || environment.DEVBOX_CONTAINER_NAME || "chatgpt-devbox-runtime",
          options.dockerProbeTimeoutSeconds,
        );
        devboxContainerRunning = inspection.exists === true && inspection.running;
      } else {
        devboxContainerRunning = false;
      }
    }

    const hostTunnelPid = await readPid(path.join(paths.runDir, "host-cloudflared.pid"));
    let tunnelRunning = hostTunnelPid ? isProcessAlive(hostTunnelPid) : null;
    if (publicEnabled && tunnelRunning === null && selectedRuntime === "docker" && dockerReady) {
      const tunnelInspection = await inspectContainer(
        environment,
        settings.CloudflaredContainerName || environment.CLOUDFLARED_CONTAINER_NAME || "chatgpt-devbox-cloudflared",
        options.dockerProbeTimeoutSeconds,
      );
      tunnelRunning = tunnelInspection.exists === true ? tunnelInspection.running : null;
    }

    // Windows host mode keeps host PowerShell elevated by default. If MCP is
    // healthy but medium-integrity, every host_exec would pop UAC via RunAs.
    // Treat that as unhealthy so elevated Guardian restarts MCP with Highest.
    const requireMcpElevated = process.platform === "win32" && selectedRuntime === "host";
    const mcpElevated = requireMcpElevated && mcpProcess.pid
      ? await isWindowsProcessElevated(mcpProcess.pid)
      : requireMcpElevated
        ? false
        : null;

    const optionalDegradations = [];
    if (publicEnabled && publicHealth && tunnelRunning === null) {
      optionalDegradations.push("public endpoint is healthy but the tunnel is externally managed");
    }
    const classified = classifyReadiness({
      shouldRun,
      selectedRuntime,
      mcpProcessRunning: Boolean(mcpProcess.pid),
      localHealth,
      publicEnabled,
      publicHealth,
      tunnelRunning,
      dockerReady,
      devboxContainerRunning,
      optionalDegradations,
      requireMcpElevated,
      mcpElevated,
    });

    return {
      ObservedAtUtc: new Date().toISOString(),
      GuardianVersion: 2,
      DesiredState: desired,
      Settings: {
        Public: publicEnabled,
        OAuth: settings.OAuth === true || !["", "none"].includes(settings.AuthMode || environment.MCP_AUTH_MODE || "none"),
        Port: port,
        PublicBaseUrl: publicBaseUrl,
        AuthMode: settings.AuthMode || environment.MCP_AUTH_MODE || "none",
        RuntimeMode: runtimeMode,
        SelectedRuntime: selectedRuntime,
        DevboxContainerName: settings.DevboxContainerName || environment.DEVBOX_CONTAINER_NAME || "chatgpt-devbox-runtime",
        CloudflaredContainerName: settings.CloudflaredContainerName || environment.CLOUDFLARED_CONTAINER_NAME || "chatgpt-devbox-cloudflared",
      },
      RuntimeMode: runtimeMode,
      SelectedRuntime: selectedRuntime,
      DockerProbePerformed: selectedRuntime === "docker" && shouldRun,
      DockerReady: dockerReady,
      DevboxRunning: devboxContainerRunning,
      CloudflaredRunning: tunnelRunning,
      HostCloudflaredProcessId: hostTunnelPid && isProcessAlive(hostTunnelPid) ? hostTunnelPid : null,
      McpProcessId: mcpProcess.pid,
      McpElevated: mcpElevated,
      RequireMcpElevated: requireMcpElevated,
      LocalHealth: localHealth,
      PublicHealth: publicHealth,
      RepairPolicy: repairPolicy,
      ...classified,
    };
  };

  const publishState = async (state, extraHeartbeat = {}) => {
    await Promise.all([
      writeJsonAtomic(paths.state, state),
      writeJsonAtomic(paths.heartbeat, {
        GuardianVersion: 2,
        ObservedAtUtc: state.ObservedAtUtc,
        GuardianPid: guardianPid,
        SupervisorPid: process.pid,
        DesiredShouldRun: state.DesiredState.ShouldRun !== false,
        IsHealthy: state.IsHealthy,
        SelectedRuntime: state.SelectedRuntime,
        McpProcessId: state.McpProcessId,
        Readiness: state.Readiness,
        Reasons: state.Reasons,
        ...extraHeartbeat,
      }),
    ]);
  };

  const repair = async (state) => {
    const repairStartedAtMs = Date.now();
    const repairStartedAtUtc = new Date(repairStartedAtMs).toISOString();
    const stamp = new Date().toISOString().replace(/[-:.TZ]/gu, "");
    const stdoutPath = path.join(paths.repairsDir, `${stamp}-stdout.log`);
    const stderrPath = path.join(paths.repairsDir, `${stamp}-stderr.log`);
    const reason = state.Reasons.join("; ");
    await log("REPAIR", `repair start (${state.SelectedRuntime}): ${reason}`);
    await publishState(state, { RepairInProgress: true, RepairReason: reason });
    let exitCode = 0;
    let timedOut = false;
    let signal = null;
    let commandElapsedMs = 0;
    let stdout = "";
    let stderr = "";
    const repairScope = process.platform === "win32" ? selectRepairScope(state) : "full";
    const repairHeartbeat = setInterval(() => {
      void writeJsonAtomic(paths.heartbeat, {
        GuardianVersion: 2,
        ObservedAtUtc: new Date().toISOString(),
        GuardianPid: guardianPid,
        SupervisorPid: process.pid,
        DesiredShouldRun: true,
        IsHealthy: false,
        SelectedRuntime: state.SelectedRuntime,
        McpProcessId: state.McpProcessId,
        Readiness: state.Readiness,
        Reasons: state.Reasons,
        RepairInProgress: true,
        RepairReason: reason,
      }).catch(() => {});
    }, Math.max(2, Math.floor(options.pollSeconds / 2)) * 1000);
    const commandStartedAtMs = Date.now();
    try {
      const result = await runRepairCommand({
        environment: { ...(await loadGuardianEnv(options.projectRoot)), DEVBOX_PROJECT_ROOT: options.projectRoot },
        settings: state.Settings,
        selectedRuntime: state.SelectedRuntime,
        repairScope,
        timeoutSeconds: 150,
      });
      exitCode = Number.isInteger(result.exitCode) ? result.exitCode : 0;
      stdout = String(result.stdout ?? "");
      stderr = String(result.stderr ?? "");
    } catch (error) {
      const failure = classifyCommandFailure(error, {
        startedAtMs: commandStartedAtMs,
        timeoutMs: 150000,
      });
      exitCode = failure.exitCode;
      timedOut = failure.timedOut;
      signal = failure.signal;
      commandElapsedMs = failure.elapsedMs;
      stdout = String(error.stdout ?? "");
      stderr = String(error.stderr ?? error.message ?? "");
    } finally {
      if (commandElapsedMs === 0) {
        commandElapsedMs = Math.max(0, Date.now() - commandStartedAtMs);
      }
      if (commandElapsedMs >= 150000) {
        timedOut = true;
        exitCode = 124;
      }
      clearInterval(repairHeartbeat);
    }
    await Promise.all([
      writeFile(stdoutPath, stdout, "utf8"),
      writeFile(stderrPath, stderr, "utf8"),
    ]);
    await sleep(3000);
    const recoveredState = await probe();
    const succeeded = !timedOut && exitCode === 0 && recoveredState.IsHealthy;
    if (state.SelectedRuntime === "docker") {
      repairPolicy = updateDockerRepairPolicy({
        policy: repairPolicy,
        succeeded,
        baseSeconds: options.dockerBackoffBaseSeconds,
        maxSeconds: options.dockerBackoffMaxSeconds,
        circuitFailureThreshold: options.dockerCircuitFailureThreshold,
        circuitOpenSeconds: options.dockerCircuitOpenSeconds,
      });
      await writeJsonAtomic(paths.repairPolicy, repairPolicy);
    }
    const completedAtMs = Date.now();
    const result = {
      AttemptedAtUtc: repairStartedAtUtc,
      CompletedAtUtc: new Date(completedAtMs).toISOString(),
      ExitCode: exitCode,
      TimedOut: timedOut,
      Signal: signal,
      ElapsedMs: Math.max(0, completedAtMs - repairStartedAtMs),
      CommandElapsedMs: commandElapsedMs,
      Reason: reason,
      Runtime: state.SelectedRuntime,
      Scope: repairScope,
      StdoutPath: stdoutPath,
      StderrPath: stderrPath,
      Succeeded: succeeded,
      RepairPolicy: repairPolicy,
    };
    await writeJsonAtomic(paths.lastRepair, result);
    const outcomeSummary = timedOut
      ? `repair timed out after ${commandElapsedMs} ms (signal ${signal ?? "none"})`
      : `repair failed or did not restore readiness (exit ${exitCode})`;
    await log(
      succeeded ? "REPAIR" : "ERROR",
      succeeded
        ? `repair completed successfully (${repairScope}, ${Math.max(0, completedAtMs - repairStartedAtMs)} ms)`
        : outcomeSummary,
    );
    recoveredState.RepairPolicy = repairPolicy;
    await publishState(recoveredState);
    return recoveredState;
  };

  const shutdown = () => {
    stopping = true;
  };
  process.once("SIGINT", shutdown);
  process.once("SIGTERM", shutdown);

  try {
    await writeFile(paths.pid, `${guardianPid}\n`, "ascii");
    await log("INFO", `guardian v2 boot supervisorPid=${process.pid} ownerPid=${guardianPid}`);
    while (!stopping) {
      let state = await probe();
      if (state.SelectedRuntime === "docker" && state.IsHealthy && repairPolicy.ConsecutiveDockerFailures > 0) {
        repairPolicy = updateDockerRepairPolicy({ policy: repairPolicy, succeeded: true });
        state.RepairPolicy = repairPolicy;
        await writeJsonAtomic(paths.repairPolicy, repairPolicy);
      }
      await publishState(state);
      if (!state.DesiredState.ShouldRun || state.IsHealthy) {
        unhealthyCount = 0;
      } else {
        unhealthyCount += 1;
        if (unhealthyCount === 1) {
          await log("WARN", `unhealthy (${state.SelectedRuntime}): ${state.Reasons.join("; ")}`);
        }
        const cooldownElapsed = Date.now() - lastRepairAtMs >= options.repairCooldownSeconds * 1000;
        const dockerAllowed = state.SelectedRuntime !== "docker" || isRepairAllowed({ policy: repairPolicy });
        if (!options.noRepair && unhealthyCount >= options.failureThreshold && cooldownElapsed && dockerAllowed) {
          state = await repair(state);
          lastRepairAtMs = Date.now();
          unhealthyCount = state.IsHealthy ? 0 : unhealthyCount;
        }
      }

      if (options.once) {
        break;
      }
      await sleep(options.pollSeconds * 1000);
    }
  } finally {
    await rm(paths.lock, { force: true });
    await log("INFO", "guardian v2 stopped");
  }
};

const runMain = () => main().catch(async (error) => {
  const options = (() => {
    try { return parseArgs(process.argv.slice(2)); } catch { return { projectRoot: defaultProjectRoot }; }
  })();
  const paths = createPaths(options.projectRoot);
  await mkdir(paths.guardianDir, { recursive: true });
  await appendFile(paths.log, `${new Date().toISOString()} [FATAL] ${error.stack || error.message}\n`, "utf8");
  console.error(error.stack || error.message);
  process.exitCode = 1;
});

if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
  runMain();
}
