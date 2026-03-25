import path from "node:path";

import { config } from "./config.js";
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

const ensureHostExecEnabled = () => {
  if (!config.enableWindowsHostExec) {
    throw new HostCommandError("Windows host command execution is disabled in the current configuration.");
  }
};

const normalizeProgram = (program) => path.win32.basename(String(program)).replace(/\.exe$/i, "").toLowerCase();

export const getHostToolStatus = () => ({
  enabled: config.enableWindowsHostExec,
  defaultWorkdir: config.hostDefaultWorkdir,
  allowlist: config.hostProgramAllowlist,
});

export const runWindowsPowerShell = async ({ command, workingDir = config.hostDefaultWorkdir, timeoutMs }) => {
  ensureHostExecEnabled();

  try {
    return await spawnProcess(
      "powershell.exe",
      ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
      {
        cwd: workingDir,
        timeoutMs,
      },
    );
  } catch (error) {
    throw wrapHostError(error, "Windows PowerShell command failed.");
  }
};

export const runAllowedProgram = async ({
  program,
  args = [],
  workingDir = config.hostDefaultWorkdir,
  timeoutMs,
}) => {
  ensureHostExecEnabled();

  const normalizedProgram = normalizeProgram(program);
  if (!config.hostProgramAllowlist.includes(normalizedProgram)) {
    throw new HostCommandError(
      `Program "${program}" is not in HOST_PROGRAM_ALLOWLIST: ${config.hostProgramAllowlist.join(", ")}`,
    );
  }

  try {
    return await spawnProcess(program, args, {
      cwd: workingDir,
      timeoutMs,
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
