import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile, rm, stat, writeFile } from "node:fs/promises";

import { config } from "./config.js";
import { detectPlatform, resolveHostShell } from "./platform.js";
import { KeyedReadWriteLock } from "./async-locks.js";
import { hashFileSha256, readLargeFileChunk, writeLargeFileMirror } from "./large-file-cli.js";
import { SpawnProcessError, spawnProcess } from "./process-utils.js";

export class HostCommandError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "HostCommandError";
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
    this.data = details.data;
    this.timedOut = details.timedOut === true;
    this.aborted = details.aborted === true;
    this.signal = details.signal ?? null;
  }
}

const wrapHostError = (error, fallbackMessage) => {
  if (error instanceof SpawnProcessError) {
    const stdout = cleanPowerShellOutput(error.stdout);
    const stderr = cleanPowerShellOutput(error.stderr);
    const message = cleanPowerShellOutput(error.message).trim() || stderr.trim() || stdout.trim() || fallbackMessage;
    return new HostCommandError(message, {
      exitCode: error.exitCode,
      stdout,
      stderr,
      data: error.data,
      timedOut: error.timedOut === true,
      aborted: error.aborted === true,
      signal: error.signal ?? null,
    });
  }

  if (error instanceof HostCommandError) {
    error.stdout = cleanPowerShellOutput(error.stdout);
    error.stderr = cleanPowerShellOutput(error.stderr);
    error.message = cleanPowerShellOutput(error.message).trim() || error.stderr.trim() || fallbackMessage;
    return error;
  }

  const message = cleanPowerShellOutput(error instanceof Error ? error.message : fallbackMessage).trim() || fallbackMessage;
  return new HostCommandError(message, {
    data: error?.data,
  });
};

const platform = detectPlatform(process.env);
const hostShell = config.hostShell || resolveHostShell(process.env, platform);

const powerShellCandidates = () => [...new Set([config.powerShellExe, config.powerShellFallbackExe].filter(Boolean))];
const isPowerShellLaunchFailure = (error) =>
  error instanceof SpawnProcessError
  && error.exitCode === null
  && error.timedOut !== true
  && error.aborted !== true;

export const spawnPowerShellProcess = async (args, options = {}) => {
  const candidates = powerShellCandidates();
  let lastError = null;
  for (let index = 0; index < candidates.length; index += 1) {
    const executable = candidates[index];
    try {
      return await spawnProcess(executable, args, options);
    } catch (error) {
      lastError = error;
      const canFallback = index + 1 < candidates.length && isPowerShellLaunchFailure(error);
      if (!canFallback) {
        throw error;
      }
    }
  }
  throw lastError ?? new HostCommandError("No usable PowerShell executable is configured.");
};

export const assertHostExecEnabled = () => {
  if (!config.enableHostExec) {
    throw new HostCommandError(`${platform.displayName} host command execution is disabled in the current configuration.`);
  }
};
const ensureHostExecEnabled = assertHostExecEnabled;

const normalizeProgram = (program) =>
  String(program)
    .split(/[\\/]/)
    .pop()
    ?.replace(/\.exe$/i, "")
    .toLowerCase() || "";
const psSingleQuote = (value) => String(value).replace(/'/g, "''");
export const MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE = 24000;
const MAX_HOST_DIAGNOSTIC_BYTES = 262144;
const MAX_HOST_DIAGNOSTIC_HASH_BYTES = 8 * 1024 * 1024;
const POWERSHELL_FILE_EXTENSIONS = new Set([".ps1", ".psm1", ".psd1"]);
const AUTOMATIC_TEXT_FILE_EXTENSIONS = new Set([
  ".bat", ".c", ".cc", ".cfg", ".cjs", ".cmd", ".conf", ".cpp", ".cs", ".css",
  ".csv", ".cxx", ".go", ".gradle", ".h", ".hpp", ".htm", ".html", ".ini", ".java",
  ".js", ".json", ".jsonc", ".jsx", ".kt", ".kts", ".less", ".md", ".mjs", ".php",
  ".pl", ".properties", ".ps1", ".psd1", ".psm1", ".py", ".rb", ".rs", ".scss", ".sh",
  ".sql", ".svelte", ".svg", ".toml", ".ts", ".tsv", ".tsx", ".txt", ".vue", ".xml",
  ".yaml", ".yml", ".zig", ".zsh",
]);
const MOJIBAKE_MARKERS = ["â€”", "â€“", "â€œ", "â€�", "â€˜", "â€™", "â€¦", "â€¢", "ðŸ", "Ã", "Â"];
const hostFileLocks = new KeyedReadWriteLock();
const POWERSHELL_QUIET_PRELUDE = [
  "$ProgressPreference = 'SilentlyContinue'",
  "$InformationPreference = 'SilentlyContinue'",
].join("\n");

const withPowerShellQuietPrelude = (command) => `${POWERSHELL_QUIET_PRELUDE}\n${String(command)}`;

export const buildWindowsPowerShellArgs = (command) => {
  const encodedCommand = Buffer.from(withPowerShellQuietPrelude(command), "utf16le").toString("base64");
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

let cachedWindowsAdminStatePromise = null;

const getCachedWindowsAdminState = async ({ workingDir = config.hostDefaultWorkdir } = {}) => {
  if (!platform.isWindows) return false;
  if (!cachedWindowsAdminStatePromise) {
    cachedWindowsAdminStatePromise = spawnPowerShellProcess(buildWindowsPowerShellArgs(buildPowerShellAdminCheckCommand()), {
      cwd: workingDir,
      timeoutMs: 15000,
    }).then((result) => Boolean(JSON.parse(result.stdout || "{}").isAdmin)).catch((error) => {
      cachedWindowsAdminStatePromise = null;
      throw error;
    });
  }
  return cachedWindowsAdminStatePromise;
};

export const warmHostExecutionState = async () => {
  if (!platform.isWindows || !config.enableHostExec) return null;
  return getCachedWindowsAdminState();
};

const awaitSharedPromiseWithSignal = async (promise, signal) => {
  if (!signal) return promise;
  if (signal.aborted) {
    const error = new Error("Command cancelled by the MCP client.");
    error.name = "AbortError";
    throw error;
  }
  let listener = null;
  const aborted = new Promise((_resolve, reject) => {
    listener = () => {
      const error = new Error("Command cancelled by the MCP client.");
      error.name = "AbortError";
      reject(error);
    };
    signal.addEventListener("abort", listener, { once: true });
  });
  try {
    return await Promise.race([promise, aborted]);
  } finally {
    if (listener) signal.removeEventListener("abort", listener);
  }
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

const countOccurrences = (value, needle) => {
  if (!value || !needle) {
    return 0;
  }

  let count = 0;
  let offset = 0;
  while (true) {
    const index = value.indexOf(needle, offset);
    if (index === -1) {
      return count;
    }
    count += 1;
    offset = index + needle.length;
  }
};

const decodePowerShellCliXmlText = (value) =>
  String(value)
    .replace(/_x([0-9a-f]{4})_/giu, (_match, hex) => String.fromCharCode(Number.parseInt(hex, 16)))
    .replace(/&lt;/gu, "<")
    .replace(/&gt;/gu, ">")
    .replace(/&quot;/gu, "\"")
    .replace(/&apos;/gu, "'")
    .replace(/&amp;/gu, "&");

export const cleanPowerShellOutput = (value) => {
  const source = String(value ?? "");
  if (!source.includes("#< CLIXML")) {
    return source;
  }

  return source.replace(/#< CLIXML\s*<Objs\b[\s\S]*?<\/Objs>/gu, (envelope) => {
    const messages = [];
    const channelPattern = /<S\s+S="(?:Error|Warning|Verbose|Debug|Information|Output)"[^>]*>([\s\S]*?)<\/S>/giu;
    for (const match of envelope.matchAll(channelPattern)) {
      const decoded = decodePowerShellCliXmlText(match[1]).trim();
      if (decoded) {
        messages.push(decoded);
      }
    }
    return messages.length > 0 ? `${messages.join("\n")}\n` : "";
  });
};

const cleanPowerShellResult = (result) => ({
  ...result,
  stdout: cleanPowerShellOutput(result?.stdout),
  stderr: cleanPowerShellOutput(result?.stderr),
});

const detectBinaryFormat = (buffer) => {
  if (buffer.length >= 2 && buffer[0] === 0x4d && buffer[1] === 0x5a) {
    return "pe";
  }
  if (buffer.length >= 4 && buffer.subarray(0, 4).equals(Buffer.from([0x7f, 0x45, 0x4c, 0x46]))) {
    return "elf";
  }
  if (buffer.length >= 4 && buffer[0] === 0x50 && buffer[1] === 0x4b && [0x03, 0x05, 0x07].includes(buffer[2])) {
    return "zip";
  }
  if (buffer.length >= 2 && buffer[0] === 0x1f && buffer[1] === 0x8b) {
    return "gzip";
  }
  if (buffer.length >= 6 && buffer.subarray(0, 6).equals(Buffer.from([0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c]))) {
    return "7z";
  }
  if (buffer.length >= 7 && buffer.subarray(0, 7).equals(Buffer.from([0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00]))) {
    return "rar";
  }
  return null;
};

const detectBom = (buffer) => {
  if (buffer.length >= 3 && buffer[0] === 0xef && buffer[1] === 0xbb && buffer[2] === 0xbf) {
    return "utf8";
  }

  if (buffer.length >= 2 && buffer[0] === 0xff && buffer[1] === 0xfe) {
    return "utf16le";
  }

  if (buffer.length >= 2 && buffer[0] === 0xfe && buffer[1] === 0xff) {
    return "utf16be";
  }

  return null;
};

const detectLineEndings = (value) => {
  const crlfCount = (value.match(/\r\n/g) ?? []).length;
  const loneLfCount = (value.match(/(?<!\r)\n/g) ?? []).length;
  const loneCrCount = (value.match(/\r(?!\n)/g) ?? []).length;

  if (crlfCount > 0 && loneLfCount === 0 && loneCrCount === 0) {
    return "crlf";
  }

  if (loneLfCount > 0 && crlfCount === 0 && loneCrCount === 0) {
    return "lf";
  }

  if (loneCrCount > 0 && crlfCount === 0 && loneLfCount === 0) {
    return "cr";
  }

  if (crlfCount === 0 && loneLfCount === 0 && loneCrCount === 0) {
    return "none";
  }

  return "mixed";
};

const sanitizePreview = (value, maxChars = 400) => String(value).slice(0, maxChars);

const resolveHostPathCandidate = (candidate, workingDir) => {
  const raw = String(candidate ?? "").trim();
  if (!raw) {
    return "";
  }

  if (/^[A-Za-z]+:\/\//.test(raw) || /^\$[A-Za-z_][A-Za-z0-9_:]*/.test(raw)) {
    return "";
  }

  if (raw.startsWith("~")) {
    return path.win32.resolve(os.homedir(), raw.slice(1));
  }

  return path.win32.isAbsolute(raw) ? path.win32.normalize(raw) : path.win32.resolve(workingDir, raw);
};

const resolveRequiredHostFilePath = (filePath, workingDir) => {
  const resolvedPath = resolveHostPathCandidate(filePath, workingDir);
  if (!resolvedPath) {
    throw new HostCommandError(`Could not resolve a Windows host path from "${filePath}".`);
  }

  return resolvedPath;
};

export const extractLikelyHostPathsFromText = ({ text, workingDir = config.hostDefaultWorkdir, limit = 6 }) => {
  const candidates = [];
  const seen = new Set();
  const source = String(text ?? "");
  const patterns = [
    /"([^"\r\n]+)"/g,
    /'([^'\r\n]+)'/g,
    /(?:^|[\s(])((?:[A-Za-z]:[\\/]|\.{1,2}[\\/]|[^"'`\s|;&]+[\\/])[^"'`\s|;&]+)/g,
  ];

  const pushCandidate = (rawValue) => {
    const resolved = resolveHostPathCandidate(rawValue, workingDir);
    if (!resolved || seen.has(resolved)) {
      return;
    }

    const extension = path.win32.extname(resolved).toLowerCase();
    const looksPathLike = /[\\/]/.test(rawValue) || /^[A-Za-z]:/.test(rawValue);
    if (!looksPathLike || !extension) {
      return;
    }

    seen.add(resolved);
    candidates.push({
      raw: rawValue,
      resolved,
    });
  };

  for (const pattern of patterns) {
    for (const match of source.matchAll(pattern)) {
      const value = match[1] ?? match[0]?.trim();
      pushCandidate(value);
      if (candidates.length >= limit) {
        return candidates;
      }
    }
  }

  return candidates;
};

const inspectPowerShellSyntax = async ({ filePath, workingDir }) => {
  const command = `
$ErrorActionPreference = 'Stop'
$path = (Resolve-Path -LiteralPath '${psSingleQuote(filePath)}').Path
$tokens = $null
$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
$result = @{
  parse_ok = (@($errors).Count -eq 0)
  error_count = @($errors).Count
  errors = @(
    @($errors) | Select-Object -First 8 | ForEach-Object {
      @{
        message = $_.Message
        line = $_.Extent.StartLineNumber
        column = $_.Extent.StartColumnNumber
        text = $_.Extent.Text
      }
    }
  )
}
[Console]::Out.Write(($result | ConvertTo-Json -Compress -Depth 6))
`;

  try {
    const result = await spawnPowerShellProcess(buildWindowsPowerShellArgs(command), {
      cwd: workingDir,
      timeoutMs: 15000,
    });
    return JSON.parse(result.stdout || "{}");
  } catch (error) {
    return {
      parse_ok: false,
      error_count: 1,
      errors: [
        {
          message: error instanceof Error ? error.message : "Failed to inspect PowerShell syntax.",
          line: null,
          column: null,
          text: null,
        },
      ],
    };
  }
};

export const inspectWindowsFile = async ({
  path: filePath,
  workingDir = config.hostDefaultWorkdir,
  maxBytes = MAX_HOST_DIAGNOSTIC_BYTES,
}) => {
  assertHostExecEnabled();

  const resolvedPath = resolveRequiredHostFilePath(filePath, workingDir);

  const extension = path.win32.extname(resolvedPath).toLowerCase();
  const fileInfo = {
    requested_path: filePath,
    resolved_path: resolvedPath,
    extension,
    exists: false,
    is_file: false,
    likely_corrupted_on_disk: false,
    syntax_invalid: false,
    observations: [],
    repair_hints: [],
  };

  let fileStat;
  try {
    fileStat = await stat(resolvedPath);
  } catch (error) {
    if (error?.code === "ENOENT") {
      fileInfo.observations.push("The path does not exist on disk.");
      fileInfo.repair_hints.push("Verify the path or recreate the file before retrying the command.");
      return fileInfo;
    }

    throw error;
  }

  fileInfo.exists = true;
  fileInfo.is_file = fileStat.isFile();
  fileInfo.size_bytes = fileStat.size;
  fileInfo.last_modified_utc = fileStat.mtime.toISOString();

  if (!fileStat.isFile()) {
    fileInfo.observations.push("The path exists but is not a regular file.");
    return fileInfo;
  }

  const chunk = await readLargeFileChunk({
    path: resolvedPath,
    offsetBytes: 0,
    maxBytes: Math.max(1, maxBytes),
  });
  const headBytes = Buffer.from(chunk.content_base64, "base64");
  const bom = detectBom(headBytes);
  const binaryFormat = detectBinaryFormat(headBytes);
  fileInfo.sampled_bytes = headBytes.length;
  fileInfo.sha256 = fileStat.size <= MAX_HOST_DIAGNOSTIC_HASH_BYTES ? await hashFileSha256(resolvedPath) : null;
  fileInfo.bom = bom;
  fileInfo.binary_format = binaryFormat;
  fileInfo.text_inspection_skipped = binaryFormat !== null;
  if (binaryFormat) {
    fileInfo.utf8_valid = null;
    fileInfo.observations.push(`Skipped text-corruption tests because the file has ${binaryFormat.toUpperCase()} binary magic.`);
    return fileInfo;
  }

  const nullByteCount = headBytes.reduce((count, byte) => count + (byte === 0 ? 1 : 0), 0);

  let strictUtf8Ok = true;
  let decodedText = "";
  try {
    decodedText = new TextDecoder("utf-8", { fatal: true }).decode(headBytes);
  } catch {
    strictUtf8Ok = false;
    decodedText = headBytes.toString("utf8");
  }

  const replacementCharacterCount = countOccurrences(decodedText, "\uFFFD");
  const mojibakeMatches = MOJIBAKE_MARKERS.filter((marker) => decodedText.includes(marker));
  const mojibakeCount = mojibakeMatches.reduce((count, marker) => count + countOccurrences(decodedText, marker), 0);

  fileInfo.utf8_valid = strictUtf8Ok;
  fileInfo.null_byte_count = nullByteCount;
  fileInfo.replacement_character_count = replacementCharacterCount;
  fileInfo.suspicious_mojibake_count = mojibakeCount;
  fileInfo.suspicious_mojibake_markers = mojibakeMatches;
  fileInfo.line_endings = detectLineEndings(decodedText);
  fileInfo.preview = sanitizePreview(decodedText);

  if (bom) {
    fileInfo.observations.push(`The file starts with a ${bom.toUpperCase()} BOM.`);
  }

  if (!strictUtf8Ok) {
    fileInfo.observations.push("The sampled bytes are not valid UTF-8.");
  }

  if (nullByteCount > 0) {
    fileInfo.observations.push(`The sampled bytes contain ${nullByteCount} NUL byte(s), which is unusual for a text source file.`);
  }

  if (mojibakeCount > 0) {
    fileInfo.observations.push(`The sampled text contains ${mojibakeCount} suspicious mojibake marker(s): ${mojibakeMatches.join(", ")}.`);
  }

  if (POWERSHELL_FILE_EXTENSIONS.has(extension)) {
    const syntax = await inspectPowerShellSyntax({ filePath: resolvedPath, workingDir });
    fileInfo.powershell_syntax = syntax;
    fileInfo.syntax_invalid = syntax.parse_ok === false;
    if (syntax.parse_ok === false) {
      fileInfo.observations.push(`PowerShell reported ${syntax.error_count} parse error(s) for this file.`);
    }
  }

  fileInfo.likely_corrupted_on_disk =
    !strictUtf8Ok || nullByteCount > 0 || mojibakeCount > 0 || replacementCharacterCount > 0;

  if (fileInfo.likely_corrupted_on_disk) {
    fileInfo.repair_hints.push("Read the exact bytes with windows_host_read_large_file before editing so you do not lose evidence of the corruption.");
    fileInfo.repair_hints.push("Rewrite the file from a clean UTF-8 or exact-byte payload with windows_host_write_large_file, then rerun the original host command.");
  } else if (fileInfo.syntax_invalid) {
    fileInfo.repair_hints.push("The file appears to be syntactically invalid but not obviously byte-corrupted; inspect the script text and fix the source logic.");
  }

  return fileInfo;
};

const maybeCollectHostCommandDiagnostics = async ({ commandText, workingDir, stdout, stderr }) => {
  const candidatePaths = extractLikelyHostPathsFromText({
    text: `${commandText ?? ""}\n${stdout ?? ""}\n${stderr ?? ""}`,
    workingDir,
  });
  if (candidatePaths.length === 0) {
    return null;
  }

  const inspectedPaths = [];
  for (const candidate of candidatePaths) {
    const extension = path.win32.extname(candidate.resolved).toLowerCase();
    if (!AUTOMATIC_TEXT_FILE_EXTENSIONS.has(extension)) {
      continue;
    }
    try {
      const candidateStat = await stat(candidate.resolved);
      if (!candidateStat.isFile()) {
        continue;
      }
      inspectedPaths.push(await inspectWindowsFile({ path: candidate.raw, workingDir }));
    } catch (error) {
      if (error?.code !== "ENOENT") {
        inspectedPaths.push({
          requested_path: candidate.raw,
          resolved_path: candidate.resolved,
          exists: null,
          inspection_error: error instanceof Error ? error.message : String(error),
        });
      }
    }
  }
  if (inspectedPaths.length === 0) {
    return null;
  }

  const suspectedFileIntegrityIssue = inspectedPaths.some((entry) => entry?.likely_corrupted_on_disk);
  const syntaxInvalidPath = inspectedPaths.some((entry) => entry?.syntax_invalid);
  const hints = [];

  if (suspectedFileIntegrityIssue) {
    hints.push("At least one referenced host file looks byte-corrupted or mojibake-encoded on disk.");
    hints.push("Use windows_host_read_large_file to capture exact bytes, then repair with windows_host_write_large_file from a clean payload.");
  } else if (syntaxInvalidPath) {
    hints.push("At least one referenced host script is syntactically invalid on disk.");
    hints.push("Use windows_host_inspect_file to review the parse errors, then repair the script before rerunning the command.");
  }

  return {
    bridge_diagnostics: {
      suspected_file_integrity_issue: suspectedFileIntegrityIssue,
      syntax_invalid_path: syntaxInvalidPath,
      inspected_paths: inspectedPaths,
      hints,
    },
  };
};

const mergeDiagnosticData = (existingData, diagnosticData) => {
  if (!diagnosticData) {
    return existingData;
  }

  if (existingData && typeof existingData === "object" && !Array.isArray(existingData)) {
    return {
      ...existingData,
      ...diagnosticData,
    };
  }

  return diagnosticData;
};

const buildHostCommandTextForDiagnostics = (program, args = []) =>
  [program, ...args].map((value) => {
    const text = String(value ?? "");
    return /[\s"'`]/.test(text) ? `"${text}"` : text;
  }).join("\n");

const writeTempPowerShellScript = async ({ tempDir, fileName, command }) => {
  const scriptPath = path.join(tempDir, fileName);
  await writeFile(scriptPath, withPowerShellQuietPrelude(command), "utf8");
  return scriptPath;
};

const isCommandTooLongError = (error) =>
  error instanceof SpawnProcessError &&
  /ENAMETOOLONG/i.test(`${error.message}\n${error.stderr}\n${error.stdout}`);

export const buildElevatedWindowsPowerShellWrapper = ({ scriptPath, workingDir, stdoutPath, stderrPath, exitCodePath }) => {
  return `
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
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

export const buildElevatedWindowsPowerShellLauncher = ({ scriptPath, workingDir, stdoutPath, stderrPath, exitCodePath, timeoutMs, powerShellExe = config.powerShellExe }) => {
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
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
$arguments = @(${escapedChildArgs})
$process = Start-Process -FilePath '${psSingleQuote(powerShellExe)}' -Verb RunAs -PassThru -WindowStyle Hidden -WorkingDirectory '${psSingleQuote(workingDir)}' -ArgumentList $arguments
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

const resolvedNodeExecutable = () => path.normalize(config.nodeExe || process.execPath || "node");

const allowWindowsHostExecUac = () =>
  ["1", "true", "yes", "on"].includes(String(process.env.ALLOW_WINDOWS_HOST_EXEC_UAC ?? "").trim().toLowerCase());

export const getHostToolStatus = () => ({
  enabled: config.enableHostExec,
  platform: config.platform.id,
  platformDisplayName: config.platform.displayName,
  shell: hostShell,
  defaultWorkdir: config.hostDefaultWorkdir,
  allowlist: config.hostProgramAllowlist,
  resolvedNodeExe: resolvedNodeExecutable(),
  powerShellExe: config.powerShellExe,
  powerShellFallbackExe: config.powerShellFallbackExe,
  powerShellFallbackEnabled: Boolean(config.powerShellFallbackExe && config.powerShellFallbackExe !== config.powerShellExe),
  // Windows host PowerShell is intended to run inside an already-elevated MCP
  // process (Highest scheduled task / Guardian repair). That avoids UAC.
  windowsHostExecDefaultsToAdmin: platform.isWindows,
  allowWindowsHostExecUac: allowWindowsHostExecUac(),
});

export const resolveHostProgramExecutable = (program) => {
  const normalizedProgram = normalizeProgram(program);
  if (normalizedProgram === "node") {
    return resolvedNodeExecutable();
  }

  return program;
};

export const readLargeFileOnHost = async ({
  path: filePath,
  offsetBytes = 0,
  maxBytes = 262144,
  workingDir = config.hostDefaultWorkdir,
}) => {
  assertHostExecEnabled();
  const resolvedPath = resolveRequiredHostFilePath(filePath, workingDir);

  return hostFileLocks.runRead(resolvedPath, () =>
    readLargeFileChunk({
      path: resolvedPath,
      offsetBytes,
      maxBytes,
    }),
  );
};

export const writeLargeFileOnHost = async ({
  path: filePath,
  contentBase64,
  append = false,
  createDirs = true,
  expectedSha256 = null,
  workingDir = config.hostDefaultWorkdir,
}) => {
  assertHostExecEnabled();
  const resolvedPath = resolveRequiredHostFilePath(filePath, workingDir);

  return hostFileLocks.runWrite(resolvedPath, () =>
    writeLargeFileMirror({
      path: resolvedPath,
      contentBase64,
      append,
      createDirs,
      expectedSha256,
    }),
  );
};

const runWindowsPowerShellFromFile = async ({ command, workingDir, timeoutMs, isAdmin, signal }) => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-hostexec-"));

  try {
    const scriptPath = await writeTempPowerShellScript({
      tempDir,
      fileName: "command.ps1",
      command,
    });

    if (isAdmin) {
      return await spawnPowerShellProcess(buildWindowsPowerShellFileArgs(scriptPath), {
        cwd: workingDir,
        timeoutMs,
        signal,
      });
    }

    const stdoutPath = path.join(tempDir, "stdout.txt");
    const stderrPath = path.join(tempDir, "stderr.txt");
    const exitCodePath = path.join(tempDir, "exitcode.txt");

    await spawnPowerShellProcess(
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
        signal,
      },
    );

    const [rawStdout, rawStderr, exitCodeText] = await Promise.all([
      readTextFileOrEmpty(stdoutPath),
      readTextFileOrEmpty(stderrPath),
      readTextFileOrEmpty(exitCodePath),
    ]);
    const stdout = cleanPowerShellOutput(rawStdout);
    const stderr = cleanPowerShellOutput(rawStderr);
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

export const runWindowsPowerShell = async ({ command, workingDir = config.hostDefaultWorkdir, timeoutMs, signal }) => {
  assertHostExecEnabled();

  try {
    const isAdmin = await awaitSharedPromiseWithSignal(getCachedWindowsAdminState({ workingDir }), signal);

    // Prefer an already-elevated MCP process. Triggering RunAs here pops a full
    // UAC secure-desktop prompt on every host_exec, which breaks unattended
    // agents. Opt in only with ALLOW_WINDOWS_HOST_EXEC_UAC=true.
    if (!isAdmin && !allowWindowsHostExecUac()) {
      throw new HostCommandError(
        "Windows host PowerShell requires the Devbox MCP process to already be elevated. "
        + "This MCP process is medium-integrity, so host_exec refused to call Start-Process -Verb RunAs (that would spam UAC). "
        + "Guardian treats unelevated MCP as unhealthy and restarts it via the Highest scheduled-task path. Retry after repair.",
        {
          exitCode: 740,
          data: {
            bridge_diagnostics: {
              suspected_elevation_gap: true,
              windows_host_exec_defaults_to_admin: true,
              allow_windows_host_exec_uac: false,
              hints: [
                "Keep MCP started only by elevated Guardian / ChatGptDevboxMcp-ElevatedStart (RunLevel Highest).",
                "Do not start MCP from a normal (non-admin) terminal if you want silent elevated host_exec.",
                "Set ALLOW_WINDOWS_HOST_EXEC_UAC=true only if you intentionally want per-command UAC prompts.",
              ],
            },
          },
        },
      );
    }

    if (shouldUsePowerShellScriptFile(command)) {
      return cleanPowerShellResult(await runWindowsPowerShellFromFile({ command, workingDir, timeoutMs, isAdmin, signal }));
    }

    if (isAdmin) {
      try {
        return cleanPowerShellResult(await spawnPowerShellProcess(buildWindowsPowerShellArgs(command), {
          cwd: workingDir,
          timeoutMs,
          signal,
        }));
      } catch (error) {
        if (isCommandTooLongError(error)) {
          return cleanPowerShellResult(await runWindowsPowerShellFromFile({ command, workingDir, timeoutMs, isAdmin, signal }));
        }
        throw error;
      }
    }

    return cleanPowerShellResult(await runWindowsPowerShellFromFile({ command, workingDir, timeoutMs, isAdmin, signal }));
  } catch (error) {
    const wrappedError = wrapHostError(error, "Windows PowerShell command failed.");
    if (signal?.aborted) {
      throw wrappedError;
    }
    const diagnosticData = await maybeCollectHostCommandDiagnostics({
      commandText: command,
      workingDir,
      stdout: wrappedError.stdout,
      stderr: wrappedError.stderr,
    });
    wrappedError.data = mergeDiagnosticData(wrappedError.data, diagnosticData);
    throw wrappedError;
  }
};

export const runHostShellCommand = async ({ command, workingDir = config.hostDefaultWorkdir, timeoutMs, signal }) => {
  ensureHostExecEnabled();

  if (platform.isWindows) {
    return runWindowsPowerShell({ command, workingDir, timeoutMs, signal });
  }

  try {
    return await spawnProcess(hostShell, ["-lc", command], {
      cwd: workingDir,
      timeoutMs,
      signal,
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
  signal,
  onStdout,
  onStderr,
  maxCaptureChars,
  onSpawn,
}) => {
  assertHostExecEnabled();

  const normalizedProgram = normalizeProgram(program);
  if (!config.hostProgramAllowlist.includes(normalizedProgram)) {
    throw new HostCommandError(
      `Program "${program}" is not in HOST_PROGRAM_ALLOWLIST: ${config.hostProgramAllowlist.join(", ")}`,
    );
  }

  try {
    const result = await spawnProcess(resolveHostProgramExecutable(program), args, {
      cwd: workingDir,
      timeoutMs,
      input,
      signal,
      onStdout,
      onStderr,
      maxCaptureChars,
      onSpawn,
    });
    return ["powershell", "pwsh"].includes(normalizedProgram) ? cleanPowerShellResult(result) : result;
  } catch (error) {
    const wrappedError = wrapHostError(error, `Host program "${program}" failed.`);
    if (signal?.aborted) {
      throw wrappedError;
    }
    const diagnosticData = await maybeCollectHostCommandDiagnostics({
      commandText: buildHostCommandTextForDiagnostics(program, args),
      workingDir,
      stdout: wrappedError.stdout,
      stderr: wrappedError.stderr,
    });
    wrappedError.data = mergeDiagnosticData(wrappedError.data, diagnosticData);
    throw wrappedError;
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
  assertHostExecEnabled();

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
