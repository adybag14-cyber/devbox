import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { mkdir, lstat, readdir, readFile, writeFile } from "node:fs/promises";

import { readLargeFileChunk, writeLargeFileMirror } from "./large-file-cli.js";
import { buildHostShellArgs, detectPlatform, resolveHostShell } from "./platform.js";
import { SpawnProcessError, spawnProcess } from "./process-utils.js";

export class HostRuntimeCommandError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "HostRuntimeCommandError";
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
  }
}

const getRuntimeConfig = (env = process.env) => {
  const platform = detectPlatform(env);
  const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
  const hostWorkspacePath = env.HOST_WORKSPACE_PATH?.trim() || path.join(projectRoot, "workspace");
  const hostDefaultWorkdir = env.HOST_DEFAULT_WORKDIR?.trim() || hostWorkspacePath || os.homedir() || projectRoot;

  return {
    platform,
    hostWorkspacePath,
    hostDefaultWorkdir,
    hostShell: resolveHostShell(env, platform),
  };
};

const wrapRuntimeError = (error, fallbackMessage) => {
  if (error instanceof HostRuntimeCommandError) {
    return error;
  }

  if (error instanceof SpawnProcessError) {
    return new HostRuntimeCommandError(error.message || fallbackMessage, {
      exitCode: error.exitCode,
      stdout: error.stdout,
      stderr: error.stderr,
    });
  }

  return new HostRuntimeCommandError(error instanceof Error ? error.message : fallbackMessage);
};

const asProcessResult = (stdout = "") => ({ stdout, stderr: "", exitCode: 0 });

const escapeRegex = (value) => String(value).replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const globToRegExp = (value) => new RegExp(`^${escapeRegex(String(value)).replace(/\\\*/g, ".*").replace(/\\\?/g, ".")}$`);

const isBinaryBuffer = (buffer) => buffer.includes(0);

const createMatcher = (pattern, caseSensitive) => {
  try {
    return new RegExp(pattern, caseSensitive ? "" : "i");
  } catch {
    const needle = caseSensitive ? String(pattern) : String(pattern).toLowerCase();
    return {
      test(value) {
        const haystack = caseSensitive ? String(value) : String(value).toLowerCase();
        return haystack.includes(needle);
      },
    };
  }
};

const entryType = (stats) => {
  if (stats.isDirectory()) {
    return "d";
  }
  if (stats.isFile()) {
    return "f";
  }
  if (stats.isSymbolicLink()) {
    return "l";
  }
  return "?";
};

const walkEntries = async ({ rootPath, recursive, maxDepth, currentDepth = 0, collected = [] }) => {
  const stats = await lstat(rootPath);
  collected.push(`${entryType(stats)}\t${rootPath}`);

  if (!stats.isDirectory()) {
    return collected;
  }

  if (!recursive || currentDepth >= Math.max(0, maxDepth)) {
    return collected;
  }

  const children = await readdir(rootPath);
  const sortedChildren = [...children].sort((left, right) => left.localeCompare(right));
  for (const child of sortedChildren) {
    await walkEntries({
      rootPath: path.join(rootPath, child),
      recursive,
      maxDepth,
      currentDepth: currentDepth + 1,
      collected,
    });
  }

  return collected;
};

const walkFiles = async ({ rootPath, matcher, maxDepth, currentDepth = 0, collected = [] }) => {
  const stats = await lstat(rootPath);
  if (stats.isFile()) {
    if (!matcher || matcher.test(path.basename(rootPath)) || matcher.test(rootPath)) {
      collected.push(rootPath);
    }
    return collected;
  }

  if (!stats.isDirectory() || currentDepth > Math.max(0, maxDepth)) {
    return collected;
  }

  const children = await readdir(rootPath);
  for (const child of [...children].sort((left, right) => left.localeCompare(right))) {
    await walkFiles({
      rootPath: path.join(rootPath, child),
      matcher,
      maxDepth,
      currentDepth: currentDepth + 1,
      collected,
    });
  }

  return collected;
};

export const getHostRuntimeInfo = async () => {
  const runtimeConfig = getRuntimeConfig();

  return {
    mode: "host",
    exists: true,
    running: true,
    status: "ready",
    name: `${runtimeConfig.platform.id}-host-runtime`,
    workspacePath: runtimeConfig.hostWorkspacePath,
    platform: runtimeConfig.platform.id,
    hostDefaultWorkdir: runtimeConfig.hostDefaultWorkdir,
    hostShell: runtimeConfig.hostShell,
  };
};

export const ensureHostRuntimeReady = async () => {
  const runtimeConfig = getRuntimeConfig();
  await mkdir(runtimeConfig.hostWorkspacePath, { recursive: true });
  return getHostRuntimeInfo();
};

export const execInHostRuntime = async ({ command, workingDir, timeoutMs }) => {
  const runtimeConfig = getRuntimeConfig();
  await ensureHostRuntimeReady();
  const cwd = workingDir || runtimeConfig.hostDefaultWorkdir;

  try {
    return await spawnProcess(runtimeConfig.hostShell, buildHostShellArgs(runtimeConfig.hostShell, command, runtimeConfig.platform), {
      cwd,
      timeoutMs,
    });
  } catch (error) {
    throw wrapRuntimeError(error, "Host runtime command failed.");
  }
};

export const execReadOnlyInHostRuntime = async ({ command, workingDir, timeoutMs }) =>
  execInHostRuntime({ command, workingDir, timeoutMs });

export const listFilesInHostRuntime = async ({ path: targetPath, recursive = false, maxDepth = 4 }) => {
  const info = await ensureHostRuntimeReady();
  const resolvedPath = targetPath || info.workspacePath;
  const entries = await walkEntries({ rootPath: resolvedPath, recursive, maxDepth: recursive ? maxDepth : 0 });
  return asProcessResult(`${entries.sort((left, right) => left.localeCompare(right)).join("\n")}\n`);
};

export const readFileInHostRuntime = async ({ path: filePath, maxBytes = 65536 }) => {
  try {
    const buffer = await readFile(filePath);
    return asProcessResult(buffer.subarray(0, Math.max(1, maxBytes)).toString("utf8"));
  } catch (error) {
    throw wrapRuntimeError(error, `Failed to read ${filePath}.`);
  }
};

export const readLargeFileInHostRuntime = async ({ path: filePath, offsetBytes = 0, maxBytes = 262144 }) =>
  readLargeFileChunk({ path: filePath, offsetBytes, maxBytes });

export const writeFileInHostRuntime = async ({ path: filePath, content, append = false, createDirs = true }) => {
  try {
    if (createDirs) {
      await mkdir(path.dirname(filePath), { recursive: true });
    }
    await writeFile(filePath, String(content), { encoding: "utf8", flag: append ? "a" : "w" });
    return asProcessResult("");
  } catch (error) {
    throw wrapRuntimeError(error, `Failed to write ${filePath}.`);
  }
};

export const writeLargeFileInHostRuntime = async ({
  path: filePath,
  contentBase64,
  append = false,
  createDirs = true,
  expectedSha256 = null,
}) =>
  writeLargeFileMirror({
    path: filePath,
    contentBase64,
    append,
    createDirs,
    expectedSha256,
  });

export const searchFilesInHostRuntime = async ({
  pattern,
  path: searchPath,
  glob = "*",
  caseSensitive = false,
  maxMatches = 200,
}) => {
  const info = await ensureHostRuntimeReady();
  const rootPath = searchPath || info.workspacePath;
  const matcher = glob ? globToRegExp(glob) : null;
  const lineMatcher = createMatcher(pattern, caseSensitive);
  const files = await walkFiles({ rootPath, matcher, maxDepth: 12 });
  const matches = [];

  for (const filePath of files) {
    if (matches.length >= Math.max(1, maxMatches)) {
      break;
    }

    const buffer = await readFile(filePath);
    if (isBinaryBuffer(buffer)) {
      continue;
    }

    const lines = buffer.toString("utf8").split(/\r?\n/);
    for (let index = 0; index < lines.length; index += 1) {
      if (lineMatcher.test(lines[index])) {
        matches.push(`${filePath}:${index + 1}:${lines[index]}`);
        if (matches.length >= Math.max(1, maxMatches)) {
          break;
        }
      }
    }
  }

  return asProcessResult(matches.join("\n") + (matches.length ? "\n" : ""));
};

export const getHostRuntimeVersions = async () => {
  const runtimeConfig = getRuntimeConfig();
  const candidates = runtimeConfig.platform.isWindows
    ? [
        ["node", ["--version"]],
        ["npm", ["--version"]],
        ["git", ["--version"]],
        ["gh", ["--version"]],
        ["python", ["--version"]],
      ]
    : [
        ["node", ["--version"]],
        ["npm", ["--version"]],
        ["git", ["--version"]],
        ["gh", ["--version"]],
        ["python3", ["--version"]],
        ["rg", ["--version"]],
      ];

  const versions = [];
  for (const [program, args] of candidates) {
    try {
      const result = await spawnProcess(program, args, { cwd: runtimeConfig.hostDefaultWorkdir, timeoutMs: 15000 });
      const line = `${program}=${`${result.stdout}${result.stderr}`.split(/\r?\n/).find(Boolean) ?? "available"}`;
      versions.push(line.trim());
    } catch {
      versions.push(`${program}=unavailable`);
    }
  }

  return versions;
};
