import { config } from "./config.js";
import { SpawnProcessError, spawnProcess } from "./process-utils.js";

const dockerBin = "docker";

const shEscape = (value) => `'${String(value).replace(/'/g, `'\"'\"'`)}'`;

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

export const getDevboxInfo = async () => {
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
    if (error instanceof DockerCommandError && /No such object/i.test(`${error.stderr}\n${error.stdout}\n${error.message}`)) {
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

export const createDevboxContainer = async () => {
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
    config.devboxImageName,
    "sleep",
    "infinity",
  ]);

  return getDevboxInfo();
};

export const ensureDevboxRunning = async () => {
  const info = await getDevboxInfo();

  if (!info.exists) {
    if (!config.devboxAutoStart) {
      throw new DockerCommandError(
        `Devbox container "${config.devboxContainerName}" does not exist and DEVBOX_AUTO_START is disabled.`,
      );
    }

    return createDevboxContainer();
  }

  if (!info.running) {
    await runDocker(["start", config.devboxContainerName]);
  }

  return getDevboxInfo();
};

export const stopDevbox = async () => {
  const info = await getDevboxInfo();
  if (!info.exists) {
    return info;
  }

  if (info.running) {
    await runDocker(["stop", config.devboxContainerName]);
  }

  return getDevboxInfo();
};

export const restartDevbox = async () => {
  const info = await getDevboxInfo();
  if (!info.exists) {
    return createDevboxContainer();
  }

  await runDocker(["restart", config.devboxContainerName]);
  return getDevboxInfo();
};

export const recreateDevbox = async () => {
  const info = await getDevboxInfo();
  if (info.exists) {
    await runDocker(["rm", "-f", config.devboxContainerName]);
  }

  return createDevboxContainer();
};

export const execInDevbox = async ({
  command,
  workingDir = config.devboxWorkspacePath,
  timeoutMs,
  user = config.devboxDefaultUser,
}) => {
  await ensureDevboxRunning();

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
  await ensureDevboxRunning();

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
  await ensureDevboxRunning();

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
  const script = [
    "import os, sys",
    "path = sys.argv[1]",
    "offset = int(sys.argv[2])",
    "max_bytes = int(sys.argv[3])",
    "if not os.path.isfile(path):",
    "    print('Not a regular file.', file=sys.stderr)",
    "    raise SystemExit(1)",
    "with open(path, 'rb') as f:",
    "    f.seek(max(0, offset))",
    "    sys.stdout.buffer.write(f.read(max(1, max_bytes)))",
  ].join("\n");

  return runProgramInDevbox({
    program: "python3",
    args: ["-c", script, path, String(Math.max(0, offsetBytes)), String(Math.max(1, maxBytes))],
    timeoutMs: 120000,
  });
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

export const writeLargeFileInDevbox = async ({ path, content, append = false, createDirs = true }) => {
  const script = [
    "import os, sys",
    "path = sys.argv[1]",
    "append = sys.argv[2] == '1'",
    "create_dirs = sys.argv[3] == '1'",
    "parent = os.path.dirname(path)",
    "if create_dirs and parent:",
    "    os.makedirs(parent, exist_ok=True)",
    "mode = 'ab' if append else 'wb'",
    "with open(path, mode) as f:",
    "    f.write(sys.stdin.buffer.read())",
  ].join("\n");

  return runProgramInDevbox({
    program: "python3",
    args: ["-c", script, path, append ? "1" : "0", createDirs ? "1" : "0"],
    input: content,
    timeoutMs: 120000,
  });
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
