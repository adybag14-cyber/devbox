import os from "node:os";
import path from "node:path";
import { mkdtemp, rm } from "node:fs/promises";

import { config } from "./config.js";
import { SpawnProcessError, spawnProcess } from "./process-utils.js";

const dockerBin = "docker";
let lifecycleTail = Promise.resolve();
let staleCleanupTail = Promise.resolve();

const shEscape = (value) => `'${String(value).replace(/'/g, `'\"'\"'`)}'`;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const devboxLargeFileReadPython = String.raw`
import base64
import hashlib
import json
import os
import stat as statmod
import sys

file_path = sys.argv[1]
offset_requested = max(0, int(sys.argv[2]))
bytes_requested = max(1, int(sys.argv[3]))
file_stat = os.stat(file_path)
if not statmod.S_ISREG(file_stat.st_mode):
    raise SystemExit("Not a regular file.")
actual_offset = min(offset_requested, file_stat.st_size)
bytes_to_read = max(0, min(bytes_requested, file_stat.st_size - actual_offset))
with open(file_path, "rb") as handle:
    handle.seek(actual_offset)
    chunk = handle.read(bytes_to_read)
result = {
    "path": file_path,
    "file_size": file_stat.st_size,
    "offset_bytes_requested": offset_requested,
    "offset_bytes": actual_offset,
    "bytes_requested": bytes_requested,
    "bytes_returned": len(chunk),
    "next_offset_bytes": actual_offset + len(chunk),
    "eof": actual_offset + len(chunk) >= file_stat.st_size,
    "content_sha256": hashlib.sha256(chunk).hexdigest(),
    "content_base64": base64.b64encode(chunk).decode("ascii"),
}
sys.stdout.write(json.dumps(result))
`;
const devboxLargeFileWritePython = String.raw`
import base64
import binascii
import hashlib
import json
import os
import re
import stat as statmod
import sys

def fail(message):
    raise SystemExit(message)

file_path = sys.argv[1]
append = sys.argv[2] == "1"
create_dirs = sys.argv[3] == "1"
expected_sha256 = sys.argv[4].strip().lower() if len(sys.argv) > 4 and sys.argv[4] else ""

stdin_base64 = sys.stdin.read()
normalized = "".join(stdin_base64.split())
if normalized:
    try:
        payload = base64.b64decode(normalized, validate=True)
    except binascii.Error:
        fail("Invalid base64 payload.")
    if base64.b64encode(payload).decode("ascii") != normalized:
        fail("Invalid base64 payload.")
else:
    payload = b""

content_sha256 = hashlib.sha256(payload).hexdigest()
if expected_sha256:
    if not re.fullmatch(r"[a-f0-9]{64}", expected_sha256):
        fail("expected_sha256 must be a 64-character SHA-256 hex string.")
    if expected_sha256 != content_sha256:
        fail("Decoded payload SHA-256 did not match expected_sha256.")

target_existed = False
previous_file_size = 0
try:
    initial_stat = os.stat(file_path)
    if not statmod.S_ISREG(initial_stat.st_mode):
        fail("Target exists but is not a regular file.")
    target_existed = True
    previous_file_size = initial_stat.st_size
except FileNotFoundError:
    pass

parent_dir = os.path.dirname(file_path)
if create_dirs and parent_dir and parent_dir != ".":
    os.makedirs(parent_dir, exist_ok=True)

with open(file_path, "ab" if append else "wb") as handle:
    handle.write(payload)

final_stat = os.stat(file_path)
if not statmod.S_ISREG(final_stat.st_mode):
    fail("Target is not a regular file after write.")

verified = False
verification_mode = ""
file_sha256 = None

if append:
    verification_mode = "suffix-bytes"
    if len(payload) == 0:
        verified = True
    else:
        with open(file_path, "rb") as handle:
            handle.seek(previous_file_size)
            verified = handle.read(len(payload)) == payload
else:
    verification_mode = "whole-file-sha256"
    digest = hashlib.sha256()
    with open(file_path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    file_sha256 = digest.hexdigest()
    verified = final_stat.st_size == len(payload) and file_sha256 == content_sha256

if not verified:
    fail("Mirror verification failed after writing the payload.")

result = {
    "path": file_path,
    "append": bool(append),
    "previous_file_size": previous_file_size,
    "final_file_size": final_stat.st_size,
    "bytes_written": len(payload),
    "content_sha256": content_sha256,
    "verification_mode": verification_mode,
    "verified": verified,
    "expected_sha256_verified": True if expected_sha256 else None,
    "target_existed": target_existed,
    "file_sha256": file_sha256,
}
sys.stdout.write(json.dumps(result))
`;

export class DockerCommandError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "DockerCommandError";
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
  }
}

const wrapDockerError = (error, fallbackMessage) => {
  if (error instanceof SpawnProcessError) {
    return new DockerCommandError(error.message || fallbackMessage, {
      exitCode: error.exitCode,
      stdout: error.stdout,
      stderr: error.stderr,
    });
  }

  return new DockerCommandError(error instanceof Error ? error.message : fallbackMessage);
};

const runDocker = async (args, options = {}) => {
  try {
    return await spawnProcess(dockerBin, args, options);
  } catch (error) {
    throw wrapDockerError(error, `Docker command failed: ${args.join(" ")}`);
  }
};

const isMissingContainerError = (error) =>
  error instanceof DockerCommandError && /No such object|No such container/i.test(`${error.stderr}\n${error.stdout}\n${error.message}`);

const waitForLifecycleIdle = async () => {
  await lifecycleTail.catch(() => {});
};

const withLifecycleLock = async (callback) => {
  const previous = lifecycleTail.catch(() => {});
  let release;
  lifecycleTail = new Promise((resolve) => {
    release = resolve;
  });

  await previous;
  try {
    return await callback();
  } finally {
    release();
  }
};

const parseStructuredStdout = (result, fallbackMessage) => {
  try {
    return JSON.parse(result.stdout);
  } catch (error) {
    throw new DockerCommandError(fallbackMessage, {
      exitCode: result.exitCode,
      stdout: result.stdout,
      stderr: result.stderr,
    });
  }
};

const getDevboxInfoNow = async () => {
  try {
    const result = await runDocker([
      "inspect",
      "--type",
      "container",
      config.devboxContainerName,
      "--format",
      "{{json .}}",
    ]);

    const data = JSON.parse(result.stdout.trim());
    return {
      exists: true,
      id: data.Id,
      image: data.Config?.Image,
      running: Boolean(data.State?.Running),
      status: data.State?.Status ?? "unknown",
      startedAt: data.State?.StartedAt ?? null,
      mounts: data.Mounts ?? [],
      name: data.Name?.replace(/^\//, "") ?? config.devboxContainerName,
    };
  } catch (error) {
    if (isMissingContainerError(error)) {
      return {
        exists: false,
        name: config.devboxContainerName,
        running: false,
        status: "missing",
      };
    }
    throw error;
  }
};

export const getDevboxInfo = async ({ waitForIdle = true } = {}) => {
  if (waitForIdle) {
    await waitForLifecycleIdle();
  }

  return getDevboxInfoNow();
};

const queueStaleContainerCleanup = (containerName, delayMs = config.devboxRetiredContainerGraceMs) => {
  staleCleanupTail = staleCleanupTail
    .catch(() => {})
    .then(async () => {
      if (delayMs > 0) {
        await sleep(delayMs);
      }

      try {
        await runDocker(["rm", "-f", containerName]);
      } catch (error) {
        if (!isMissingContainerError(error)) {
          // Surface cleanup failures in logs without breaking the active request path.
          console.error(`[docker-runtime] failed to clean up retired container ${containerName}:`, error);
        }
      }
    });
};

const removeContainerIfPresent = async (containerName) => {
  try {
    await runDocker(["rm", "-f", containerName]);
  } catch (error) {
    if (!isMissingContainerError(error)) {
      throw error;
    }
  }
};

const hasManagedTmpVolume = (info) =>
  Array.isArray(info?.mounts) &&
  info.mounts.some((mount) => mount?.Destination === "/tmp" && mount?.Type === "volume" && mount?.Name === config.devboxTmpVolumeName);

const copyContainerPathViaHost = async ({ sourceContainer, sourcePath, targetContainer, targetPath }) => {
  const stagingRoot = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-tmp-"));
  const stagingPath = path.join(stagingRoot, "payload");

  try {
    await runDocker(["cp", `${sourceContainer}:${sourcePath}`, stagingPath]);
    await runDocker(["cp", `${stagingPath}${path.sep}.`, `${targetContainer}:${targetPath}`]);
  } finally {
    await rm(stagingRoot, { recursive: true, force: true }).catch(() => {});
  }
};

const createDevboxContainerNow = async () => {
  await runDocker([
    "run",
    "-d",
    "--name",
    config.devboxContainerName,
    "--init",
    "-w",
    config.devboxWorkspacePath,
    "-v",
    `${config.hostWorkspacePath}:${config.devboxWorkspacePath}`,
    "-v",
    `${config.devboxTmpVolumeName}:/tmp`,
    config.devboxImageName,
    "sleep",
    "infinity",
  ]);

  return getDevboxInfoNow();
};

const ensureDevboxRunningNow = async () => {
  const info = await getDevboxInfoNow();

  if (!info.exists) {
    if (!config.devboxAutoStart) {
      throw new DockerCommandError(
        `Devbox container "${config.devboxContainerName}" does not exist and DEVBOX_AUTO_START is disabled.`,
      );
    }

    return createDevboxContainerNow();
  }

  if (!info.running) {
    await runDocker(["start", config.devboxContainerName]);
  }

  return getDevboxInfoNow();
};

export const ensureDevboxRunning = async ({ waitForIdle = true } = {}) => {
  if (waitForIdle) {
    await waitForLifecycleIdle();
  }

  return ensureDevboxRunningNow();
};

export const stopDevbox = async () =>
  withLifecycleLock(async () => {
    const info = await getDevboxInfoNow();
    if (!info.exists) {
      return info;
    }

    if (info.running) {
      await runDocker(["stop", config.devboxContainerName]);
    }

    return getDevboxInfoNow();
  });

export const restartDevbox = async () =>
  withLifecycleLock(async () => {
    const info = await getDevboxInfoNow();
    if (!info.exists) {
      return createDevboxContainerNow();
    }

    await runDocker(["restart", config.devboxContainerName]);
    return getDevboxInfoNow();
  });

export const recreateDevbox = async () =>
  withLifecycleLock(async () => {
    const info = await getDevboxInfoNow();
    let retiredContainerName = null;
    const needsTmpMigration = info.exists && !hasManagedTmpVolume(info);

    if (info.exists) {
      retiredContainerName = `${config.devboxContainerName}-retired-${Date.now()}-${Math.random().toString(16).slice(2, 8)}`;
      await runDocker(["rename", config.devboxContainerName, retiredContainerName]);
    }

    try {
      const recreatedInfo = await createDevboxContainerNow();
      if (retiredContainerName && needsTmpMigration) {
        await copyContainerPathViaHost({
          sourceContainer: retiredContainerName,
          sourcePath: "/tmp",
          targetContainer: config.devboxContainerName,
          targetPath: "/tmp",
        });
      }
      if (retiredContainerName) {
        queueStaleContainerCleanup(retiredContainerName);
      }
      return recreatedInfo;
    } catch (error) {
      if (retiredContainerName) {
        await removeContainerIfPresent(config.devboxContainerName);
        try {
          await runDocker(["rename", retiredContainerName, config.devboxContainerName]);
        } catch (restoreError) {
          throw new DockerCommandError(
            `${error instanceof Error ? error.message : "Failed to recreate the Docker devbox."} Rollback also failed: ${
              restoreError instanceof Error ? restoreError.message : String(restoreError)
            }`,
          );
        }
      }

      throw error;
    }
  });

export const execInDevbox = async ({
  command,
  workingDir = config.devboxWorkspacePath,
  timeoutMs,
  user = config.devboxDefaultUser,
}) => {
  await waitForLifecycleIdle();
  await ensureDevboxRunningNow();

  const args = ["exec", "-i"];
  if (user) {
    args.push("-u", user);
  }
  args.push("-w", workingDir, config.devboxContainerName, "bash", "-lc", command);
  return runDocker(args, { timeoutMs });
};

export const execReadOnlyInDevbox = async ({
  command,
  workingDir = config.devboxWorkspacePath,
  timeoutMs,
  user = config.devboxDefaultUser,
}) => {
  await waitForLifecycleIdle();
  await ensureDevboxRunningNow();

  const args = [
    "run",
    "--rm",
    "-i",
    "--read-only",
    "--network",
    "none",
    "--cap-drop",
    "ALL",
    "--security-opt",
    "no-new-privileges",
    "--tmpfs",
    "/tmp:rw,noexec,nosuid,size=64m",
    "--tmpfs",
    "/run:rw,noexec,nosuid,size=16m",
    "--tmpfs",
    "/var/tmp:rw,noexec,nosuid,size=64m",
  ];

  if (user) {
    args.push("-u", user);
  }

  args.push(
    "-w",
    workingDir,
    "-v",
    `${config.hostWorkspacePath}:${config.devboxWorkspacePath}:ro`,
    config.devboxImageName,
    "bash",
    "-lc",
    command,
  );

  return runDocker(args, { timeoutMs });
};

export const runProgramInDevbox = async ({
  program,
  args = [],
  workingDir = config.devboxWorkspacePath,
  timeoutMs,
  user = config.devboxDefaultUser,
  input,
}) => {
  await waitForLifecycleIdle();
  await ensureDevboxRunningNow();

  const dockerArgs = ["exec", "-i"];
  if (user) {
    dockerArgs.push("-u", user);
  }

  dockerArgs.push("-w", workingDir, config.devboxContainerName, program, ...args);
  return runDocker(dockerArgs, { timeoutMs, input });
};

export const getDevboxVersions = async () => {
  const result = await execInDevbox({
    command: [
      "printf 'gh='; gh --version | head -n 1",
      "printf 'node='; node --version",
      "printf 'npm='; npm --version",
      "printf 'python='; python3 --version",
      "printf 'git='; git --version",
      "printf 'rg='; rg --version | head -n 1",
    ].join(" && "),
    timeoutMs: 20000,
  });

  return result.stdout
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean);
};

export const listFilesInDevbox = async ({
  path = config.devboxWorkspacePath,
  recursive = false,
  maxDepth = 4,
}) => {
  const command = recursive
    ? `find ${shEscape(path)} -maxdepth ${Math.max(1, maxDepth)} \\( -type d -o -type f -o -type l \\) -printf '%y\\t%p\\n' | sort`
    : `find ${shEscape(path)} -maxdepth 1 \\( -type d -o -type f -o -type l \\) -printf '%y\\t%p\\n' | sort`;

  return execInDevbox({ command, timeoutMs: 30000 });
};

export const readFileInDevbox = async ({ path, maxBytes = 65536 }) =>
  execInDevbox({
    command: `if [ ! -f ${shEscape(path)} ]; then echo 'Not a regular file.' >&2; exit 1; fi; head -c ${Math.max(1, maxBytes)} -- ${shEscape(path)}`,
    timeoutMs: 30000,
  });

export const readLargeFileInDevbox = async ({ path, offsetBytes = 0, maxBytes = 262144 }) => {
  const result = await runProgramInDevbox({
    program: "python3",
    args: ["-c", devboxLargeFileReadPython, path, String(Math.max(0, offsetBytes)), String(Math.max(1, maxBytes))],
    timeoutMs: 120000,
  });

  return parseStructuredStdout(result, `Large file read for ${path} returned invalid JSON.`);
};

export const writeFileInDevbox = async ({ path, content, append = false, createDirs = true }) => {
  const encoded = Buffer.from(content, "utf8").toString("base64");
  const op = append ? ">>" : ">";
  const prelude = createDirs ? `mkdir -p -- "$(dirname -- ${shEscape(path)})" && ` : "";

  return execInDevbox({
    command: `${prelude}printf '%s' ${shEscape(encoded)} | base64 -d ${op} ${shEscape(path)}`,
    timeoutMs: 30000,
  });
};

export const writeLargeFileInDevbox = async ({
  path,
  contentBase64,
  append = false,
  createDirs = true,
  expectedSha256 = null,
}) => {
  const result = await runProgramInDevbox({
    program: "python3",
    args: ["-c", devboxLargeFileWritePython, path, append ? "1" : "0", createDirs ? "1" : "0", expectedSha256 ?? ""],
    input: contentBase64,
    timeoutMs: 120000,
  });

  return parseStructuredStdout(result, `Large file write for ${path} returned invalid JSON.`);
};

export const searchFilesInDevbox = async ({
  pattern,
  path = config.devboxWorkspacePath,
  glob = "*",
  caseSensitive = false,
  maxMatches = 200,
}) =>
  execInDevbox({
    command: [
      "rg",
      caseSensitive ? "-n" : "-ni",
      "--glob",
      shEscape(glob),
      "-m",
      String(Math.max(1, maxMatches)),
      "--",
      shEscape(pattern),
      shEscape(path),
    ].join(" "),
    timeoutMs: 30000,
  });

export const getDevboxGithubAuthStatus = async () => {
  await ensureDevboxRunning();

  const statusResult = await runProgramInDevbox({
    program: "gh",
    args: ["auth", "status", "--hostname", "github.com"],
    timeoutMs: 15000,
  });

  const userNameResult = await execInDevbox({
    command: "git config --global --get user.name || true",
    timeoutMs: 5000,
  });

  const userEmailResult = await execInDevbox({
    command: "git config --global --get user.email || true",
    timeoutMs: 5000,
  });

  return {
    statusSummary: `${statusResult.stdout}${statusResult.stderr}`.trim(),
    userName: userNameResult.stdout.trim(),
    userEmail: userEmailResult.stdout.trim(),
  };
};

export const syncGithubAuthToDevbox = async ({ token, userName, userEmail }) => {
  if (!token) {
    throw new DockerCommandError("A GitHub token is required to sync auth into the devbox.");
  }

  await ensureDevboxRunning();

  await runProgramInDevbox({
    program: "gh",
    args: ["auth", "login", "--hostname", "github.com", "--with-token"],
    input: `${token}\n`,
    timeoutMs: 20000,
  });

  await runProgramInDevbox({
    program: "gh",
    args: ["auth", "setup-git", "--hostname", "github.com"],
    timeoutMs: 15000,
  });

  if (userName) {
    await runProgramInDevbox({
      program: "git",
      args: ["config", "--global", "user.name", userName],
      timeoutMs: 5000,
    });
  }

  if (userEmail) {
    await runProgramInDevbox({
      program: "git",
      args: ["config", "--global", "user.email", userEmail],
      timeoutMs: 5000,
    });
  }

  return getDevboxGithubAuthStatus();
};
