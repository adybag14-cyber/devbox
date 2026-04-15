import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";

import { config } from "./config.js";
import { detectPlatform, resolveHostShell } from "./platform.js";
import { SpawnProcessError, spawnProcess } from "./process-utils.js";

export class HostCommandError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "HostCommandError";
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
  }
}

const wrapHostError = (error, fallbackMessage) => {
  if (error instanceof SpawnProcessError) {
    return new HostCommandError(error.message || fallbackMessage, {
      exitCode: error.exitCode,
      stdout: error.stdout,
      stderr: error.stderr,
    });
  }

  return new HostCommandError(error instanceof Error ? error.message : fallbackMessage);
};

const platform = detectPlatform(process.env);
const hostShell = resolveHostShell(process.env, platform);

const ensureHostExecEnabled = () => {
  if (!config.enableHostExec) {
    throw new HostCommandError(`${platform.displayName} host command execution is disabled in the current configuration.`);
  }
};

const normalizeProgram = (program) =>
  String(program)
    .split(/[\\/]/)
    .pop()
    ?.replace(/\.exe$/i, "")
    .toLowerCase() || "";
const psSingleQuote = (value) => String(value).replace(/'/g, "''");
export const MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE = 24000;

export const buildWindowsPowerShellArgs = (command) => {
  const encodedCommand = Buffer.from(String(command), "utf16le").toString("base64");
  return ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encodedCommand];
};

export const buildWindowsPowerShellFileArgs = (scriptPath) => [
  "-NoLogo",
  "-NoProfile",
  "-NonInteractive",
  "-ExecutionPolicy",
  "Bypass",
  "-File",
  scriptPath,
];

export const shouldUsePowerShellScriptFile = (command) => {
  const totalChars = buildWindowsPowerShellArgs(command).join(" ").length;
  return totalChars >= MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE;
};

const buildPowerShellAdminCheckCommand = () => `
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
[Console]::Out.Write((@{
  isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} | ConvertTo-Json -Compress))
`;

const readTextFileOrEmpty = async (filePath) => {
  try {
    return await readFile(filePath, "utf8");
  } catch (error) {
    if (error && error.code === "ENOENT") {
      return "";
    }

    throw error;
  }
};

const writeTempPowerShellScript = async ({ tempDir, fileName, command }) => {
  const scriptPath = path.join(tempDir, fileName);
  await writeFile(scriptPath, String(command), "utf8");
  return scriptPath;
};

const isCommandTooLongError = (error) =>
  error instanceof SpawnProcessError &&
  /ENAMETOOLONG/i.test(`${error.message}\n${error.stderr}\n${error.stdout}`);

export const buildElevatedWindowsPowerShellWrapper = ({ scriptPath, workingDir, stdoutPath, stderrPath, exitCodePath }) => {
  return `
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$stdoutPath = '${psSingleQuote(stdoutPath)}'
$stderrPath = '${psSingleQuote(stderrPath)}'
$exitCodePath = '${psSingleQuote(exitCodePath)}'
Set-Location -LiteralPath '${psSingleQuote(workingDir)}'
$global:LASTEXITCODE = 0
try {
  & '${psSingleQuote(scriptPath)}' 1> $stdoutPath 2> $stderrPath 3>> $stdoutPath 4>> $stdoutPath 5>> $stdoutPath 6>> $stdoutPath
  $exitCode = if ($global:LASTEXITCODE -is [int]) { [int]$global:LASTEXITCODE } else { 0 }
} catch {
  $_ | Out-File -LiteralPath $stderrPath -Encoding utf8 -Append
  if ($_.ScriptStackTrace) {
    $_.ScriptStackTrace | Out-File -LiteralPath $stderrPath -Encoding utf8 -Append
  }
  $exitCode = 1
}
Set-Content -LiteralPath $exitCodePath -Value ([string]$exitCode) -Encoding ascii
exit $exitCode
`;
};

export const buildElevatedWindowsPowerShellLauncher = ({ scriptPath, workingDir, stdoutPath, stderrPath, exitCodePath, timeoutMs }) => {
  const childArgs = buildWindowsPowerShellArgs(
    buildElevatedWindowsPowerShellWrapper({
      scriptPath,
      workingDir,
      stdoutPath,
      stderrPath,
      exitCodePath,
    }),
  );

  const escapedChildArgs = childArgs.map((arg) => `'${psSingleQuote(arg)}'`).join(", ");

  return `
$ErrorActionPreference = 'Stop'
$arguments = @(${escapedChildArgs})
$process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -PassThru -WindowStyle Hidden -WorkingDirectory '${psSingleQuote(workingDir)}' -ArgumentList $arguments
if ($null -eq $process) {
  throw 'Failed to start elevated PowerShell process.'
}
if (-not $process.WaitForExit(${Math.max(1, timeoutMs)})) {
  try {
    Stop-Process -Id $process.Id -Force -ErrorAction Stop
  } catch {
  }
  throw 'Command timed out after ${Math.max(1, timeoutMs)} ms.'
}
exit $process.ExitCode
`;
};

export const getHostToolStatus = () => ({
  enabled: config.enableHostExec,
  platform: config.platform.id,
  platformDisplayName: config.platform.displayName,
  shell: hostShell,
  defaultWorkdir: config.hostDefaultWorkdir,
  allowlist: config.hostProgramAllowlist,
  resolvedNodeExe: config.nodeExe,
  windowsHostExecDefaultsToAdmin: platform.isWindows,
});

export const resolveHostProgramExecutable = (program) => {
  const normalizedProgram = normalizeProgram(program);
  if (normalizedProgram === "node") {
    return config.nodeExe;
  }

  return program;
};

const runWindowsPowerShellFromFile = async ({ command, workingDir, timeoutMs, isAdmin }) => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-hostexec-"));

  try {
    const scriptPath = await writeTempPowerShellScript({
      tempDir,
      fileName: "command.ps1",
      command,
    });

    if (isAdmin) {
      return await spawnProcess("powershell.exe", buildWindowsPowerShellFileArgs(scriptPath), {
        cwd: workingDir,
        timeoutMs,
      });
    }

    const stdoutPath = path.join(tempDir, "stdout.txt");
    const stderrPath = path.join(tempDir, "stderr.txt");
    const exitCodePath = path.join(tempDir, "exitcode.txt");

    await spawnProcess(
      "powershell.exe",
      buildWindowsPowerShellArgs(
        buildElevatedWindowsPowerShellLauncher({
          scriptPath,
          workingDir,
          stdoutPath,
          stderrPath,
          exitCodePath,
          timeoutMs: timeoutMs ?? 300000,
        }),
      ),
      {
        cwd: workingDir,
        timeoutMs: (timeoutMs ?? 300000) + 15000,
      },
    );

    const [stdout, stderr, exitCodeText] = await Promise.all([
      readTextFileOrEmpty(stdoutPath),
      readTextFileOrEmpty(stderrPath),
      readTextFileOrEmpty(exitCodePath),
    ]);
    const parsedExitCode = Number.parseInt(String(exitCodeText).trim() || "0", 10);
    const exitCode = Number.isFinite(parsedExitCode) ? parsedExitCode : 0;

    if (exitCode !== 0) {
      throw new HostCommandError(stderr.trim() || stdout.trim() || "Windows PowerShell command failed.", {
        exitCode,
        stdout,
        stderr,
      });
    }

    return {
      stdout,
      stderr,
      exitCode,
    };
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
};

export const runWindowsPowerShell = async ({ command, workingDir = config.hostDefaultWorkdir, timeoutMs }) => {
  ensureHostExecEnabled();

  try {
    const adminCheck = await spawnProcess("powershell.exe", buildWindowsPowerShellArgs(buildPowerShellAdminCheckCommand()), {
      cwd: workingDir,
      timeoutMs: Math.min(timeoutMs ?? 15000, 15000),
    });

    const isAdmin = Boolean(JSON.parse(adminCheck.stdout || "{}").isAdmin);
    if (shouldUsePowerShellScriptFile(command)) {
      return await runWindowsPowerShellFromFile({ command, workingDir, timeoutMs, isAdmin });
    }

    if (isAdmin) {
      try {
        return await spawnProcess("powershell.exe", buildWindowsPowerShellArgs(command), {
          cwd: workingDir,
          timeoutMs,
        });
      } catch (error) {
        if (isCommandTooLongError(error)) {
          return await runWindowsPowerShellFromFile({ command, workingDir, timeoutMs, isAdmin });
        }
        throw error;
      }
    }

    return await runWindowsPowerShellFromFile({ command, workingDir, timeoutMs, isAdmin });
  } catch (error) {
    throw wrapHostError(error, "Windows PowerShell command failed.");
  }
};

export const runHostShellCommand = async ({ command, workingDir = config.hostDefaultWorkdir, timeoutMs }) => {
  ensureHostExecEnabled();

  if (platform.isWindows) {
    return runWindowsPowerShell({ command, workingDir, timeoutMs });
  }

  try {
    return await spawnProcess(hostShell, ["-lc", command], {
      cwd: workingDir,
      timeoutMs,
    });
  } catch (error) {
    throw wrapHostError(error, `${platform.displayName} host shell command failed.`);
  }
};

export const runAllowedProgram = async ({
  program,
  args = [],
  workingDir = config.hostDefaultWorkdir,
  timeoutMs,
  input,
}) => {
  ensureHostExecEnabled();

  const normalizedProgram = normalizeProgram(program);
  if (!config.hostProgramAllowlist.includes(normalizedProgram)) {
    throw new HostCommandError(
      `Program "${program}" is not in HOST_PROGRAM_ALLOWLIST: ${config.hostProgramAllowlist.join(", ")}`,
    );
  }

  try {
    return await spawnProcess(resolveHostProgramExecutable(program), args, {
      cwd: workingDir,
      timeoutMs,
      input,
    });
  } catch (error) {
    throw wrapHostError(error, `Host program "${program}" failed.`);
  }
};

const tryAllowedProgram = async (options) => {
  try {
    return await runAllowedProgram(options);
  } catch (error) {
    if (error instanceof HostCommandError) {
      return null;
    }
    throw error;
  }
};

export const getHostGithubAuthContext = async () => {
  ensureHostExecEnabled();

  const statusResult = await runAllowedProgram({
    program: "gh",
    args: ["auth", "status", "--hostname", "github.com"],
    timeoutMs: 15000,
  });

  const tokenResult = await runAllowedProgram({
    program: "gh",
    args: ["auth", "token", "--hostname", "github.com"],
    timeoutMs: 15000,
  });

  const userNameResult = await tryAllowedProgram({
    program: "git",
    args: ["config", "--global", "user.name"],
    timeoutMs: 5000,
  });

  const userEmailResult = await tryAllowedProgram({
    program: "git",
    args: ["config", "--global", "user.email"],
    timeoutMs: 5000,
  });

  const token = tokenResult.stdout.trim();
  if (!token) {
    throw new HostCommandError("Host GitHub CLI did not return a token.");
  }

  return {
    token,
    statusSummary: `${statusResult.stdout}${statusResult.stderr}`.trim(),
    userName: userNameResult?.stdout?.trim() || "",
    userEmail: userEmailResult?.stdout?.trim() || "",
  };
};
