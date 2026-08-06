import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { mkdir, lstat, readdir, readFile, writeFile } from "node:fs/promises";

import { config } from "./config.js";
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

  const explicitHostShell = env.HOST_SHELL?.trim() || "";
  const explicitPowerShell = env.POWERSHELL_EXE?.trim() || "";
  const hostShell = explicitHostShell
    || (platform.isWindows ? explicitPowerShell || config.powerShellExe : resolveHostShell(env, platform));
  const hostShellFallback = platform.isWindows && !explicitHostShell
    ? env.POWERSHELL_FALLBACK_EXE?.trim() || config.powerShellFallbackExe
    : "";

  return {
    platform,
    hostWorkspacePath,
    hostDefaultWorkdir,
    hostShell,
    hostShellFallback,
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

const isRuntimeShellLaunchFailure = (error) =>
  error instanceof SpawnProcessError
  && error.exitCode === null
  && error.timedOut !== true
  && error.aborted !== true;

const spawnRuntimeShell = async ({ runtimeConfig, command, cwd, timeoutMs, signal, onStdout, onStderr, maxCaptureChars }) => {
  const candidates = [...new Set([runtimeConfig.hostShell, runtimeConfig.hostShellFallback].filter(Boolean))];
  let lastError = null;
  for (let index = 0; index < candidates.length; index += 1) {
    const shell = candidates[index];
    try {
      return await spawnProcess(shell, buildHostShellArgs(shell, command, runtimeConfig.platform), {
        cwd,
        timeoutMs,
        signal,
        onStdout,
        onStderr,
        maxCaptureChars,
      });
    } catch (error) {
      lastError = error;
      const canFallback = index + 1 < candidates.length && isRuntimeShellLaunchFailure(error);
      if (!canFallback) {
        throw error;
      }
    }
  }
  throw lastError ?? new HostRuntimeCommandError("No usable host shell is configured.");
};

const asProcessResult = (stdout = "", stderr = "") => ({ stdout, stderr, exitCode: 0 });
const DEFAULT_PRUNED_DIRECTORIES = [".git", "node_modules", ".cache", ".venv", "venv", "__pycache__"];

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

const throwIfAborted = (signal) => {
  if (signal?.aborted) {
    const error = new Error("Recursive filesystem operation cancelled by the MCP client.");
    error.name = "AbortError";
    throw error;
  }
};

const traversalShouldStop = (state, signal) => {
  throwIfAborted(signal);
  if (Date.now() >= state.deadlineMs) {
    state.timedOut = true;
    return true;
  }
  return state.truncated === true;
};

const normalizePrunedDirectories = (values = DEFAULT_PRUNED_DIRECTORIES) =>
  new Set(values.map((value) => String(value).trim().toLowerCase()).filter(Boolean));

const walkEntries = async ({
  rootPath,
  recursive,
  maxDepth,
  maxEntries,
  deadlineMs,
  signal,
  prunedDirectories,
  currentDepth = 0,
  collected = [],
  state = { deadlineMs, truncated: false, timedOut: false, pruned: 0, skipped: 0 },
}) => {
  if (traversalShouldStop(state, signal)) {
    return { collected, state };
  }
  let stats;
  try {
    stats = await lstat(rootPath);
  } catch (error) {
    if (["EACCES", "ENOENT", "EPERM"].includes(error?.code)) {
      state.skipped += 1;
      return { collected, state };
    }
    throw error;
  }
  collected.push(`${entryType(stats)}\t${rootPath}`);
  if (collected.length >= maxEntries) {
    state.truncated = true;
    return { collected, state };
  }

  if (!stats.isDirectory()) {
    return { collected, state };
  }

  if (!recursive || currentDepth >= Math.max(0, maxDepth)) {
    return { collected, state };
  }

  let children;
  try {
    children = await readdir(rootPath);
  } catch (error) {
    if (["EACCES", "ENOENT", "EPERM"].includes(error?.code)) {
      state.skipped += 1;
      return { collected, state };
    }
    throw error;
  }
  const sortedChildren = [...children].sort((left, right) => left.localeCompare(right));
  for (const child of sortedChildren) {
    if (recursive && prunedDirectories.has(child.toLowerCase())) {
      state.pruned += 1;
      continue;
    }
    await walkEntries({
      rootPath: path.join(rootPath, child),
      recursive,
      maxDepth,
      maxEntries,
      deadlineMs,
      signal,
      prunedDirectories,
      currentDepth: currentDepth + 1,
      collected,
      state,
    });
    if (traversalShouldStop(state, signal)) {
      break;
    }
  }

  return { collected, state };
};

const searchFileTree = async ({
  rootPath,
  matcher,
  lineMatcher,
  maxDepth,
  maxMatches,
  maxFiles,
  maxBytesPerFile,
  signal,
  prunedDirectories,
  currentDepth = 0,
  state,
}) => {
  if (traversalShouldStop(state, signal) || state.matches.length >= maxMatches) {
    return;
  }

  let stats;
  try {
    stats = await lstat(rootPath);
  } catch (error) {
    if (["EACCES", "ENOENT", "EPERM"].includes(error?.code)) {
      state.skipped += 1;
      return;
    }
    throw error;
  }

  if (stats.isFile()) {
    if (state.filesScanned >= maxFiles) {
      state.truncated = true;
      return;
    }
    if (matcher && !matcher.test(path.basename(rootPath)) && !matcher.test(rootPath)) {
      return;
    }
    if (stats.size > maxBytesPerFile) {
      state.skippedLarge += 1;
      return;
    }

    state.filesScanned += 1;
    const buffer = await readFile(rootPath);
    if (isBinaryBuffer(buffer)) {
      state.skippedBinary += 1;
      return;
    }
    const lines = buffer.toString("utf8").split(/\r?\n/);
    for (let index = 0; index < lines.length; index += 1) {
      throwIfAborted(signal);
      if (lineMatcher.test(lines[index])) {
        state.matches.push(`${rootPath}:${index + 1}:${lines[index]}`);
        if (state.matches.length >= maxMatches) {
          state.truncated = true;
          return;
        }
      }
    }
    return;
  }

  if (!stats.isDirectory() || currentDepth >= Math.max(0, maxDepth)) {
    return;
  }

  let children;
  try {
    children = await readdir(rootPath);
  } catch (error) {
    if (["EACCES", "ENOENT", "EPERM"].includes(error?.code)) {
      state.skipped += 1;
      return;
    }
    throw error;
  }
  for (const child of [...children].sort((left, right) => left.localeCompare(right))) {
    if (prunedDirectories.has(child.toLowerCase())) {
      state.pruned += 1;
      continue;
    }
    await searchFileTree({
      rootPath: path.join(rootPath, child),
      matcher,
      lineMatcher,
      maxDepth,
      maxMatches,
      maxFiles,
      maxBytesPerFile,
      signal,
      prunedDirectories,
      currentDepth: currentDepth + 1,
      state,
    });
    if (traversalShouldStop(state, signal) || state.matches.length >= maxMatches) {
      return;
    }
  }
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
    hostShellFallback: runtimeConfig.hostShellFallback || null,
  };
};

export const ensureHostRuntimeReady = async () => {
  const runtimeConfig = getRuntimeConfig();
  await mkdir(runtimeConfig.hostWorkspacePath, { recursive: true });
  return getHostRuntimeInfo();
};

export const execInHostRuntime = async ({ command, workingDir, timeoutMs, signal, onStdout, onStderr, maxCaptureChars }) => {
  const runtimeConfig = getRuntimeConfig();
  await ensureHostRuntimeReady();
  const cwd = workingDir || runtimeConfig.hostDefaultWorkdir;

  try {
    return await spawnRuntimeShell({ runtimeConfig, command, cwd, timeoutMs, signal, onStdout, onStderr, maxCaptureChars });
  } catch (error) {
    throw wrapRuntimeError(error, "Host runtime command failed.");
  }
};

export const execReadOnlyInHostRuntime = async ({ command, workingDir, timeoutMs, signal, onStdout, onStderr, maxCaptureChars }) =>
  execInHostRuntime({ command, workingDir, timeoutMs, signal, onStdout, onStderr, maxCaptureChars });

export const listFilesInHostRuntime = async ({
  path: targetPath,
  recursive = false,
  maxDepth = 4,
  maxEntries = 5000,
  timeoutMs = 30000,
  excludeDirectories = DEFAULT_PRUNED_DIRECTORIES,
  signal,
}) => {
  const info = await ensureHostRuntimeReady();
  const resolvedPath = targetPath || info.workspacePath;
  const { collected, state } = await walkEntries({
    rootPath: resolvedPath,
    recursive,
    maxDepth: recursive ? maxDepth : 0,
    maxEntries: Math.max(1, maxEntries),
    deadlineMs: Date.now() + Math.max(1, timeoutMs),
    signal,
    prunedDirectories: normalizePrunedDirectories(excludeDirectories),
  });
  const notices = [];
  if (state.timedOut) notices.push(`listing stopped after ${timeoutMs} ms`);
  if (state.truncated) notices.push(`listing capped at ${Math.max(1, maxEntries)} entries`);
  if (state.pruned > 0) notices.push(`pruned ${state.pruned} excluded directories`);
  if (state.skipped > 0) notices.push(`skipped ${state.skipped} inaccessible or vanished paths`);
  return asProcessResult(`${collected.join("\n")}\n`, notices.length ? `${notices.join("; ")}\n` : "");
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
  maxDepth = 12,
  maxFiles = 10000,
  maxBytesPerFile = 2 * 1024 * 1024,
  timeoutMs = 30000,
  excludeDirectories = DEFAULT_PRUNED_DIRECTORIES,
  signal,
}) => {
  const info = await ensureHostRuntimeReady();
  const rootPath = searchPath || info.workspacePath;
  const matcher = glob ? globToRegExp(glob) : null;
  const lineMatcher = createMatcher(pattern, caseSensitive);
  const state = {
    deadlineMs: Date.now() + Math.max(1, timeoutMs),
    matches: [],
    filesScanned: 0,
    pruned: 0,
    skipped: 0,
    skippedLarge: 0,
    skippedBinary: 0,
    truncated: false,
    timedOut: false,
  };
  await searchFileTree({
    rootPath,
    matcher,
    lineMatcher,
    maxDepth: Math.max(0, maxDepth),
    maxMatches: Math.max(1, maxMatches),
    maxFiles: Math.max(1, maxFiles),
    maxBytesPerFile: Math.max(1, maxBytesPerFile),
    signal,
    prunedDirectories: normalizePrunedDirectories(excludeDirectories),
    state,
  });
  const notices = [];
  if (state.timedOut) notices.push(`search stopped after ${timeoutMs} ms`);
  if (state.matches.length >= Math.max(1, maxMatches)) notices.push(`match limit ${Math.max(1, maxMatches)} reached`);
  if (state.filesScanned >= Math.max(1, maxFiles)) notices.push(`file scan limit ${Math.max(1, maxFiles)} reached`);
  if (state.pruned > 0) notices.push(`pruned ${state.pruned} excluded directories`);
  if (state.skipped > 0) notices.push(`skipped ${state.skipped} inaccessible or vanished paths`);
  if (state.skippedLarge > 0) notices.push(`skipped ${state.skippedLarge} oversized files`);

  return asProcessResult(
    state.matches.join("\n") + (state.matches.length ? "\n" : ""),
    notices.length ? `${notices.join("; ")}\n` : "",
  );
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
