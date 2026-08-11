#!/usr/bin/env node

import { existsSync } from "node:fs";
import { appendFile, mkdir, open, readFile, readdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { execFile, spawn } from "node:child_process";
import { promisify } from "node:util";
import { fileURLToPath, pathToFileURL } from "node:url";

import {
  classifyReadiness,
  classifyStartupActivity,
  classifyTunnelTransport,
  deriveCloudflaredMetricDeltas,
  isRepairAllowed,
  normalizeRuntimeMode,
  resolveSelectedRuntime,
  resolveFailureThreshold,
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

const GUARDIAN_LOG_MAX_BYTES = 4 * 1024 * 1024;
const GUARDIAN_LOG_ROTATIONS = 3;
const REPAIR_LOG_FILE_LIMIT = 100;

const rotateLogIfNeeded = async (filePath, maxBytes = GUARDIAN_LOG_MAX_BYTES, rotations = GUARDIAN_LOG_ROTATIONS) => {
  const current = await stat(filePath).catch(() => null);
  if (!current || current.size < maxBytes || rotations < 1) return;
  await rm(`${filePath}.${rotations}`, { force: true }).catch(() => {});
  for (let index = rotations - 1; index >= 1; index -= 1) {
    await rename(`${filePath}.${index}`, `${filePath}.${index + 1}`).catch(() => {});
  }
  await rename(filePath, `${filePath}.1`).catch(() => {});
};

const appendRotatingLog = async (filePath, text) => {
  await rotateLogIfNeeded(filePath);
  await appendFile(filePath, text, "utf8");
};

const pruneRepairLogs = async (directory, limit = REPAIR_LOG_FILE_LIMIT) => {
  const entries = await readdir(directory, { withFileTypes: true }).catch(() => []);
  const files = [];
  for (const entry of entries) {
    if (!entry.isFile() || !/-(?:stdout|stderr)\.log$/u.test(entry.name)) continue;
    const fullPath = path.join(directory, entry.name);
    const metadata = await stat(fullPath).catch(() => null);
    if (metadata) files.push({ fullPath, mtimeMs: metadata.mtimeMs });
  }
  files.sort((left, right) => right.mtimeMs - left.mtimeMs);
  await Promise.all(files.slice(limit).map(({ fullPath }) => rm(fullPath, { force: true }).catch(() => {})));
};

const parseArgs = (argv) => {
  const options = {
    projectRoot: process.env.DEVBOX_PROJECT_ROOT || defaultProjectRoot,
    pollSeconds: 10,
    failureThreshold: 3,
    liveMcpFailureThreshold: 6,
    repairCooldownSeconds: 120,
    repairFailureBackoffSeconds: 300,
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
    ["--live-mcp-failure-threshold", "liveMcpFailureThreshold"],
    ["--repair-cooldown-seconds", "repairCooldownSeconds"],
    ["--repair-failure-backoff-seconds", "repairFailureBackoffSeconds"],
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
    (repairScope === "public-tunnel" || settings.Public) ? "-Public" : "",
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

export const restoreRepairBackoff = ({ lastRepair = null, currentMcpPid = null, nowMs = Date.now() } = {}) => {
  const persistedPid = Number.parseInt(lastRepair?.RepairBackoffMcpProcessId ?? "", 10);
  const activePid = Number.parseInt(currentMcpPid ?? "", 10);
  const deadlineMs = Date.parse(String(lastRepair?.RepairBackoffUntilUtc ?? ""));
  if (
    !Number.isInteger(persistedPid) || persistedPid < 1 ||
    !Number.isInteger(activePid) || activePid !== persistedPid ||
    !Number.isFinite(deadlineMs) || deadlineMs <= nowMs
  ) {
    return 0;
  }
  return deadlineMs;
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
 * Returns true/false when the target Windows token is verified, or null when
 * token inspection itself fails. Non-Windows platforms always report true.
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
    return null;
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

const readMcpPerformanceState = async (projectRoot, environment = process.env) => {
  const configured = String(environment.MCP_PERFORMANCE_STATE_PATH ?? "").trim();
  const statePath = configured
    ? (path.isAbsolute(configured) ? configured : path.resolve(projectRoot, configured))
    : path.join(projectRoot, "run", "mcp-performance.json");
  return readJson(statePath, null);
};


export const sampleWindowsHostPressure = async (environment = process.env) => {
  if (process.platform !== "win32") return null;
  const powerShell = resolveWindowsPowerShellExecutable(environment);
  const command = String.raw`
$ErrorActionPreference='Stop'
$os = Get-CimInstance Win32_OperatingSystem
$cpu = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
$memory = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory -ErrorAction SilentlyContinue
$page = @(Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue)
$pageAllocatedMb = ($page | Measure-Object -Property AllocatedBaseSize -Sum).Sum
$pageUsedMb = ($page | Measure-Object -Property CurrentUsage -Sum).Sum
[Console]::Out.Write((([ordered]@{
  SampledAtUtc = [DateTime]::UtcNow.ToString('o')
  CpuPercent = if($null -ne $cpu){[double]$cpu}else{$null}
  TotalPhysicalBytes = [int64]$os.TotalVisibleMemorySize * 1024
  FreePhysicalBytes = [int64]$os.FreePhysicalMemory * 1024
  TotalVirtualBytes = [int64]$os.TotalVirtualMemorySize * 1024
  FreeVirtualBytes = [int64]$os.FreeVirtualMemory * 1024
  CommittedBytes = if($memory){[int64]$memory.CommittedBytes}else{$null}
  CommitLimitBytes = if($memory){[int64]$memory.CommitLimit}else{$null}
  PageFileAllocatedBytes = if($null -ne $pageAllocatedMb){[int64]$pageAllocatedMb * 1MB}else{$null}
  PageFileUsedBytes = if($null -ne $pageUsedMb){[int64]$pageUsedMb * 1MB}else{$null}
} | ConvertTo-Json -Compress)))
`;
  try {
    const result = await execFileAsync(powerShell, ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command], {
      encoding: "utf8",
      timeout: 8000,
      windowsHide: true,
      maxBuffer: 256 * 1024,
    });
    return JSON.parse(String(result.stdout ?? "{}").trim() || "{}");
  } catch (error) {
    return { SampledAtUtc: new Date().toISOString(), Error: error instanceof Error ? error.message : String(error) };
  }
};

const sumPrometheusMetric = (text, metricName) => {
  let total = 0;
  let found = false;
  for (const line of String(text ?? "").split(/\r?\n/u)) {
    if (!line || line.startsWith("#")) continue;
    const match = line.match(/^([^ {]+)(?:\{[^}]*\})?\s+([-+0-9.eE]+)$/u);
    if (!match || match[1] !== metricName) continue;
    const value = Number(match[2]);
    if (!Number.isFinite(value)) continue;
    total += value;
    found = true;
  }
  return found ? total : null;
};

const readCloudflaredMetrics = async (environment, timeoutSeconds = 2) => {
  const url = String(environment.CLOUDFLARED_METRICS_URL ?? "http://127.0.0.1:20241/metrics").trim();
  if (!url) return null;
  try {
    const response = await fetch(url, { signal: AbortSignal.timeout(timeoutSeconds * 1000) });
    if (!response.ok) return { Url: url, Reachable: false, HttpStatus: response.status };
    const text = await response.text();
    const names = [
      "cloudflared_tunnel_ha_connections",
      "cloudflared_tunnel_request_errors",
      "cloudflared_tunnel_total_requests",
      "cloudflared_quic_client_closed_connections",
      "quic_client_closed_connections",
    ];
    const values = Object.fromEntries(names.map((name) => [name, sumPrometheusMetric(text, name)]));
    return {
      Url: url,
      Reachable: true,
      HaConnections: values.cloudflared_tunnel_ha_connections,
      RequestErrors: values.cloudflared_tunnel_request_errors,
      TotalRequests: values.cloudflared_tunnel_total_requests,
      QuicClosedConnections: values.cloudflared_quic_client_closed_connections ?? values.quic_client_closed_connections,
    };
  } catch (error) {
    return { Url: url, Reachable: false, Error: error instanceof Error ? error.message : String(error) };
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

  if (repairScope === "public-tunnel") {
    const helper = path.join(environment.DEVBOX_PROJECT_ROOT, "scripts", "restart-cloudflare-tunnel.sh");
    if (existsSync(helper)) {
      try {
        return await runProcessUntilExit(environment.SHELL || "/bin/sh", [helper, "auto"], {
          cwd: environment.DEVBOX_PROJECT_ROOT,
          env: environment,
          encoding: "utf8",
          timeout: timeoutSeconds * 1000,
          maxBuffer: 4 * 1024 * 1024,
        });
      } catch (error) {
        // If the tunnel was not installed through Devbox's known service
        // managers, fall back to the ordinary runtime restart below. Preserve
        // the helper failure as context in the final stderr when possible.
        environment.DEVBOX_TUNNEL_RESTART_FALLBACK_REASON = String(error.stderr ?? error.message ?? "");
      }
    }
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
    startupState: path.join(runDir, "startup-state.json"),
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
  await pruneRepairLogs(paths.repairsDir);
  if (!(await acquireGuardianLock(paths.lock, options.projectRoot))) {
    return;
  }

  const ownerPid = Number.parseInt(process.env.DEVBOX_GUARDIAN_WRAPPER_PID ?? "", 10);
  const guardianPid = Number.isInteger(ownerPid) && ownerPid > 0 ? ownerPid : process.pid;
  let stopping = false;
  let unhealthyCount = 0;
  let lastRepairAtMs = 0;
  const [persistedLastRepair, initialMcpProcess] = await Promise.all([
    readJson(paths.lastRepair, null),
    findMcpProcess(paths.runDir),
  ]);
  let repairBackoffUntilMs = restoreRepairBackoff({
    lastRepair: persistedLastRepair,
    currentMcpPid: initialMcpProcess.pid,
  });
  let repairPolicy = await readJson(paths.repairPolicy, {});
  let mcpElevationCache = { pid: null, elevated: null };
  let lastCloudflaredSample = { key: null, metrics: null };
  let lastDeferredStartupAttemptId = null;
  let lastHeartbeatState = null;
  let heartbeatExtra = {};
  let lastHostPressureSampleMs = 0;
  let hostPressureSample = null;
  let hostPressureSamplePromise = null;

  const log = async (level, message) => {
    await appendRotatingLog(paths.log, `${new Date().toISOString()} [${level}] ${message}\n`);
  };

  const probe = async () => {
    const [environment, settingsValue, desiredValue, startupStateValue] = await Promise.all([
      loadGuardianEnv(options.projectRoot),
      readJson(paths.settings, {}),
      readJson(paths.desired, { ShouldRun: true, Source: "devbox-guardian.mjs" }),
      readJson(paths.startupState, null),
    ]);
    environment.DEVBOX_PROJECT_ROOT = options.projectRoot;
    const settings = settingsValue ?? {};
    const desired = desiredValue ?? { ShouldRun: true };
    const shouldRun = desired.ShouldRun !== false;
    const startupPid = Number.parseInt(startupStateValue?.ProcessId ?? "", 10);
    const startupActivity = classifyStartupActivity({
      startupState: startupStateValue,
      processAlive: Number.isInteger(startupPid) && startupPid > 0 ? isProcessAlive(startupPid) : false,
    });
    const port = Number.parseInt(settings.Port ?? environment.PORT ?? "8100", 10) || 8100;
    const publicBaseUrl = normalizePublicUrl(settings.PublicBaseUrl || environment.PUBLIC_BASE_URL || environment.CLOUDFLARED_PUBLIC_HOSTNAME);
    const publicEnabled = settings.Public === true || Boolean(publicBaseUrl);
    const mcpProcess = await findMcpProcess(paths.runDir);
    const localBaseUrl = `http://127.0.0.1:${port}`;
    const [localHealth, publicHealth, mcpPerformance] = await Promise.all([
      testHealth(`${localBaseUrl}/healthz`),
      publicEnabled ? testHealth(`${publicBaseUrl}/healthz`) : Promise.resolve(null),
      readMcpPerformanceState(options.projectRoot, environment),
    ]);
    const pressureIntervalMs = Math.max(10000, Number.parseInt(environment.GUARDIAN_HOST_PRESSURE_SAMPLE_MS ?? "60000", 10) || 60000);
    if (
      process.platform === "win32"
      && !hostPressureSamplePromise
      && (hostPressureSample === null || Date.now() - lastHostPressureSampleMs >= pressureIntervalMs)
    ) {
      lastHostPressureSampleMs = Date.now();
      hostPressureSamplePromise = sampleWindowsHostPressure(environment)
        .then((sample) => {
          hostPressureSample = sample;
          return sample;
        })
        .catch((error) => {
          hostPressureSample = {
            SampledAtUtc: new Date().toISOString(),
            Error: error instanceof Error ? error.message : String(error),
          };
          return hostPressureSample;
        })
        .finally(() => {
          hostPressureSamplePromise = null;
        });
    }
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
    let mcpElevated = null;
    if (requireMcpElevated && mcpProcess.pid) {
      if (mcpElevationCache.pid === mcpProcess.pid && typeof mcpElevationCache.elevated === "boolean") {
        mcpElevated = mcpElevationCache.elevated;
      } else {
        const observedElevation = await isWindowsProcessElevated(mcpProcess.pid);
        if (typeof observedElevation === "boolean") {
          mcpElevationCache = { pid: mcpProcess.pid, elevated: observedElevation };
          mcpElevated = observedElevation;
        } else {
          // Probe failures are unknown, not evidence of a medium-integrity MCP.
          mcpElevationCache = { pid: mcpProcess.pid, elevated: null };
          mcpElevated = null;
        }
      }
    } else if (requireMcpElevated) {
      mcpElevationCache = { pid: null, elevated: null };
      mcpElevated = false;
    } else {
      mcpElevationCache = { pid: null, elevated: null };
    }

    const cloudflaredMetrics = publicEnabled && tunnelRunning !== false
      ? await readCloudflaredMetrics(environment)
      : null;
    const cloudflaredSampleKey = hostTunnelPid
      ? `pid:${hostTunnelPid}`
      : selectedRuntime === "docker"
        ? `docker:${settings.CloudflaredContainerName || environment.CLOUDFLARED_CONTAINER_NAME || "chatgpt-devbox-cloudflared"}`
        : "externally-managed";
    const previousCloudflaredMetrics = lastCloudflaredSample.key === cloudflaredSampleKey
      ? lastCloudflaredSample.metrics
      : null;
    const cloudflaredMetricsDelta = deriveCloudflaredMetricDeltas({
      previous: previousCloudflaredMetrics,
      current: cloudflaredMetrics,
    });
    if (cloudflaredMetrics) {
      lastCloudflaredSample = { key: cloudflaredSampleKey, metrics: cloudflaredMetrics };
    } else if (tunnelRunning === false) {
      lastCloudflaredSample = { key: null, metrics: null };
    }
    const tunnelTransport = classifyTunnelTransport({
      publicEnabled,
      tunnelRunning,
      metrics: cloudflaredMetrics,
    });
    const optionalDegradations = [];
    if (publicEnabled && cloudflaredMetrics && cloudflaredMetrics.Reachable === false) {
      optionalDegradations.push("cloudflared metrics endpoint is unavailable");
    }
    if (publicEnabled && publicHealth && tunnelRunning === null) {
      optionalDegradations.push("public endpoint is healthy but the tunnel is externally managed");
    }
    if (cloudflaredMetricsDelta?.QuicClosedConnections > 0) {
      optionalDegradations.push(
        `cloudflared observed ${cloudflaredMetricsDelta.QuicClosedConnections} newly closed QUIC connection(s) since the previous probe`,
      );
    }
    const classified = classifyReadiness({
      shouldRun,
      selectedRuntime,
      mcpProcessRunning: Boolean(mcpProcess.pid),
      localHealth,
      publicEnabled,
      publicHealth,
      tunnelRunning,
      tunnelTransportHealthy: tunnelTransport.Healthy,
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
        NamedTunnel: Boolean(environment.CLOUDFLARED_TUNNEL_TOKEN && environment.CLOUDFLARED_PUBLIC_HOSTNAME),
      },
      RuntimeMode: runtimeMode,
      SelectedRuntime: selectedRuntime,
      StartupState: startupStateValue,
      StartupInProgress: startupActivity.Active,
      StartupActivity: startupActivity,
      DockerProbePerformed: selectedRuntime === "docker" && shouldRun,
      DockerReady: dockerReady,
      DevboxRunning: devboxContainerRunning,
      CloudflaredRunning: tunnelRunning,
      HostCloudflaredProcessId: hostTunnelPid && isProcessAlive(hostTunnelPid) ? hostTunnelPid : null,
      CloudflaredMetrics: cloudflaredMetrics,
      CloudflaredMetricsDelta: cloudflaredMetricsDelta,
      TunnelTransportHealthy: tunnelTransport.Healthy,
      TunnelTransportDegraded: tunnelTransport.Degraded,
      TunnelTransportReasons: tunnelTransport.Reasons,
      McpProcessId: mcpProcess.pid,
      McpElevated: mcpElevated,
      RequireMcpElevated: requireMcpElevated,
      LocalHealth: localHealth,
      PublicHealth: publicHealth,
      McpPerformance: mcpPerformance,
      HostPressure: hostPressureSample,
      RepairPolicy: repairPolicy,
      ...classified,
    };
  };

  const heartbeatPayload = (state, extra = {}) => ({
    GuardianVersion: 2,
    // The watchdog heartbeat timestamp reflects liveness now, not when the
    // potentially slow health probe began.
    ObservedAtUtc: new Date().toISOString(),
    GuardianPid: guardianPid,
    SupervisorPid: process.pid,
    DesiredShouldRun: state?.DesiredState?.ShouldRun !== false,
    IsHealthy: state?.IsHealthy ?? false,
    SelectedRuntime: state?.SelectedRuntime ?? null,
    McpProcessId: state?.McpProcessId ?? null,
    StartupInProgress: state?.StartupInProgress ?? false,
    StartupPhase: state?.StartupActivity?.Phase ?? null,
    Readiness: state?.Readiness ?? null,
    Reasons: state?.Reasons ?? [],
    ...extra,
  });

  const publishState = async (state, extraHeartbeat = {}) => {
    lastHeartbeatState = state;
    heartbeatExtra = extraHeartbeat;
    await Promise.all([
      writeJsonAtomic(paths.state, state),
      writeJsonAtomic(paths.heartbeat, heartbeatPayload(state, extraHeartbeat)),
    ]);
  };

  // Keep watchdog liveness independent from public health, PowerShell token
  // inspection, Docker, or any other probe that can block. This prevents the
  // external KeepAlive task from killing a healthy Guardian during a slow probe.
  const heartbeatTimer = setInterval(() => {
    if (!lastHeartbeatState) return;
    void writeJsonAtomic(paths.heartbeat, heartbeatPayload(lastHeartbeatState, heartbeatExtra)).catch(() => {});
  }, 5000);
  heartbeatTimer.unref?.();

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
    const repairScope = selectRepairScope(state);
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
    if (succeeded) {
      repairBackoffUntilMs = 0;
    } else if (Number.parseInt(recoveredState.McpProcessId ?? "", 10) > 0) {
      repairBackoffUntilMs = Date.now() + options.repairFailureBackoffSeconds * 1000;
    } else {
      repairBackoffUntilMs = 0;
    }
    if (state.SelectedRuntime === "docker" && repairScope === "full") {
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
      RepairBackoffUntilUtc: repairBackoffUntilMs > Date.now()
        ? new Date(repairBackoffUntilMs).toISOString()
        : null,
      RepairBackoffMcpProcessId: repairBackoffUntilMs > Date.now()
        ? recoveredState.McpProcessId
        : null,
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

      if (
        state.IsHealthy &&
        state.StartupActivity?.Stale === true &&
        state.StartupState?.Status === "running"
      ) {
        const recoveredStartupState = {
          ...state.StartupState,
          Phase: "ready",
          Status: "recovered-stale",
          UpdatedAtUtc: new Date().toISOString(),
          Details: "Guardian observed a healthy MCP after the recorded startup owner became stale.",
          RecoveredByGuardian: true,
        };
        await writeJsonAtomic(paths.startupState, recoveredStartupState);
        await log(
          "INFO",
          `reconciled stale startup journal attempt=${state.StartupActivity?.AttemptId ?? "unknown"} phase=${state.StartupActivity?.Phase ?? "unknown"}`,
        );
      }

      if (!state.DesiredState.ShouldRun || state.IsHealthy) {
        unhealthyCount = 0;
        repairBackoffUntilMs = 0;
        lastDeferredStartupAttemptId = null;
      } else if (state.StartupInProgress) {
        unhealthyCount = 0;
        const attemptId = state.StartupActivity?.AttemptId ?? "unknown";
        if (lastDeferredStartupAttemptId !== attemptId) {
          await log(
            "INFO",
            `startup in progress; deferring repair attempt=${attemptId} pid=${state.StartupActivity?.ProcessId ?? "unknown"} phase=${state.StartupActivity?.Phase ?? "unknown"}`,
          );
          lastDeferredStartupAttemptId = attemptId;
        }
      } else {
        lastDeferredStartupAttemptId = null;
        unhealthyCount += 1;
        if (unhealthyCount === 1) {
          await log("WARN", `unhealthy (${state.SelectedRuntime}): ${state.Reasons.join("; ")}`);
        }
        const cooldownElapsed = Date.now() - lastRepairAtMs >= options.repairCooldownSeconds * 1000;
        const failureBackoffElapsed = Date.now() >= repairBackoffUntilMs;
        const plannedRepairScope = selectRepairScope(state);
        const namedTunnelRepairIndependentOfDocker = plannedRepairScope === "public-tunnel" && state.Settings?.NamedTunnel === true;
        const dockerAllowed = namedTunnelRepairIndependentOfDocker || state.SelectedRuntime !== "docker" || isRepairAllowed({ policy: repairPolicy });
        // Missing MCP processes and confirmed tunnel transport failures recover
        // quickly. A verified live MCP that temporarily misses health probes
        // gets a longer grace window so host/Hyper-V scheduler pressure is not
        // amplified into a destructive restart loop.
        const effectiveFailureThreshold = resolveFailureThreshold({
          state,
          configuredThreshold: options.failureThreshold,
          liveMcpFailureThreshold: options.liveMcpFailureThreshold,
        });
        if (
          !options.noRepair &&
          unhealthyCount >= effectiveFailureThreshold &&
          cooldownElapsed &&
          failureBackoffElapsed &&
          dockerAllowed
        ) {
          // Re-probe immediately before any repair. A busy host can recover between
          // the observation that crossed the threshold and the destructive action.
          // Always repair from the fresh state so a recovered MCP turns a would-be
          // full restart into either no repair or a tunnel-only repair.
          const freshState = await probe();
          await publishState(freshState);
          if (freshState.IsHealthy) {
            await log("INFO", "repair cancelled after fresh pre-repair probe recovered readiness");
            state = freshState;
            unhealthyCount = 0;
          } else if (freshState.StartupInProgress) {
            await log(
              "INFO",
              `repair deferred after fresh probe observed startup attempt=${freshState.StartupActivity?.AttemptId ?? "unknown"} phase=${freshState.StartupActivity?.Phase ?? "unknown"}`,
            );
            state = freshState;
            unhealthyCount = 0;
          } else {
            const freshRepairScope = selectRepairScope(freshState);
            const freshFailureThreshold = resolveFailureThreshold({
              state: freshState,
              configuredThreshold: options.failureThreshold,
              liveMcpFailureThreshold: options.liveMcpFailureThreshold,
            });
            const freshNamedTunnelRepairIndependentOfDocker =
              freshRepairScope === "public-tunnel" && freshState.Settings?.NamedTunnel === true;
            const freshDockerAllowed =
              freshNamedTunnelRepairIndependentOfDocker ||
              freshState.SelectedRuntime !== "docker" ||
              isRepairAllowed({ policy: repairPolicy });
            const freshCooldownElapsed = Date.now() - lastRepairAtMs >= options.repairCooldownSeconds * 1000;
            const freshFailureBackoffElapsed = Date.now() >= repairBackoffUntilMs;

            if (freshRepairScope === "full" && freshRepairScope !== plannedRepairScope) {
              await log(
                "INFO",
                `repair deferred because fresh scope changed from ${plannedRepairScope} to full; rebuilding failure evidence`,
              );
              state = freshState;
              unhealthyCount = 1;
            } else if (
              unhealthyCount < freshFailureThreshold ||
              !freshCooldownElapsed ||
              !freshFailureBackoffElapsed ||
              !freshDockerAllowed
            ) {
              state = freshState;
            } else {
              state = await repair(freshState);
              lastRepairAtMs = Date.now();
              unhealthyCount = state.IsHealthy ? 0 : unhealthyCount;
            }
          }
        }
      }

      if (options.once) {
        break;
      }
      await sleep(options.pollSeconds * 1000);
    }
  } finally {
    clearInterval(heartbeatTimer);
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
  await appendRotatingLog(paths.log, `${new Date().toISOString()} [FATAL] ${error.stack || error.message}\n`);
  console.error(error.stack || error.message);
  process.exitCode = 1;
});

if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
  runMain();
}
