import os from "node:os";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { loadEnvFile } from "./env.js";
import { defaultHostProgramAllowlist, detectPlatform, mergeHostProgramAllowlist, resolveHostShell, resolveRuntimeMode } from "./platform.js";

const parseInteger = (value, fallback) => {
  const parsed = Number.parseInt(value ?? "", 10);
  return Number.isFinite(parsed) ? parsed : fallback;
};

const isUnlimitedLimitValue = (value) => {
  const normalized = String(value ?? "").trim().toLowerCase();
  return ["0", "-1", "none", "off", "disabled", "unlimited", "infinite", "infinity"].includes(normalized);
};

const parseCharacterLimit = (value, fallback) => {
  if (value === undefined || value === null || String(value).trim() === "") {
    return fallback;
  }

  if (isUnlimitedLimitValue(value)) {
    return null;
  }

  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
};

export const MAX_SAFE_COMMAND_OUTPUT_CHARS = 65536;
export const DEFAULT_MCP_TRANSFER_CHARS = 4000000;
export const MIN_SAFE_MCP_TRANSFER_CHARS = 262144;

const parseCommandOutputLimit = (value) => {
  const parsed = Number.parseInt(value ?? "", 10);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return MAX_SAFE_COMMAND_OUTPUT_CHARS;
  }
  return Math.max(100, Math.min(parsed, MAX_SAFE_COMMAND_OUTPUT_CHARS));
};

const parseMcpTransferLimit = (value) => {
  const parsed = parseCharacterLimit(value, DEFAULT_MCP_TRANSFER_CHARS);
  return parsed === null ? null : Math.max(MIN_SAFE_MCP_TRANSFER_CHARS, parsed);
};

const parseJsonBodyLimit = (value, fallback) => {
  if (value === undefined || value === null || String(value).trim() === "") {
    return fallback;
  }

  if (isUnlimitedLimitValue(value)) {
    return Number.MAX_SAFE_INTEGER;
  }

  return String(value).trim();
};

const parseBoolean = (value, fallback = false) => {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }

  const normalized = String(value).trim().toLowerCase();
  return ["1", "true", "yes", "on"].includes(normalized);
};

const parseCsv = (value, fallback = []) =>
  (value === undefined || value === null || String(value).trim() === "" ? fallback : String(value)
  )
    .toString()
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean)
    .filter((item, index, items) => items.indexOf(item) === index);

const normalizeUrl = (value) => {
  const rawValue = String(value ?? "").trim();
  if (!rawValue) {
    return "";
  }

  if (/^https?:\/\//i.test(rawValue)) {
    return rawValue.replace(/\/+$/, "");
  }

  return `https://${rawValue.replace(/\/+$/, "")}`;
};

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
await loadEnvFile(path.join(projectRoot, ".env"));
const rawPublicBaseUrl = process.env.PUBLIC_BASE_URL?.trim();
const publicBaseUrl = rawPublicBaseUrl ? rawPublicBaseUrl.replace(/\/+$/, "") : "";
const platform = detectPlatform(process.env);
const runtimeMode = resolveRuntimeMode({ requestedMode: process.env.DEVBOX_RUNTIME_MODE, platform });
const defaultHostWorkspacePath = path.join(projectRoot, "workspace");
const hostWorkspacePath = process.env.HOST_WORKSPACE_PATH?.trim() || defaultHostWorkspacePath;
const defaultOauthStateFilePath = path.join(projectRoot, "run", "oauth-state.json");
const defaultHostWorkdir = process.env.HOST_DEFAULT_WORKDIR?.trim() || hostWorkspacePath || os.homedir() || projectRoot;
const defaultNodeExe = process.execPath || "node";
const defaultDevboxWorkspacePath = runtimeMode === "host" ? hostWorkspacePath : "/workspace";
const enableHostExec = parseBoolean(process.env.ENABLE_HOST_EXEC ?? process.env.ENABLE_WINDOWS_HOST_EXEC, true);
const legacyWindowsPowerShellExe = path.join(process.env.SystemRoot || "C:\\Windows", "System32", "WindowsPowerShell", "v1.0", "powershell.exe");
const installedPowerShell7Exe = path.join(process.env.ProgramFiles || "C:\\Program Files", "PowerShell", "7", "pwsh.exe");
const configuredPowerShellExe = process.env.POWERSHELL_EXE?.trim() || "";
const configuredPowerShellFallbackExe = process.env.POWERSHELL_FALLBACK_EXE?.trim() || "";
const isUsablePowerShellCandidate = (candidate) => Boolean(candidate) && (!path.isAbsolute(candidate) || existsSync(candidate));
const powerShellExe = platform.isWindows
  ? [configuredPowerShellExe, installedPowerShell7Exe, legacyWindowsPowerShellExe].find(isUsablePowerShellCandidate) || "powershell.exe"
  : "";
const powerShellFallbackExe = platform.isWindows
  ? [configuredPowerShellFallbackExe, legacyWindowsPowerShellExe].find(isUsablePowerShellCandidate) || "powershell.exe"
  : "";
const hostShell = process.env.HOST_SHELL?.trim() || (platform.isWindows ? powerShellExe : resolveHostShell(process.env, platform));
const hostProgramDefaults = defaultHostProgramAllowlist(platform);
const hostProgramConfigured = parseCsv(process.env.HOST_PROGRAM_ALLOWLIST, []);
const hostProgramExtra = parseCsv(process.env.HOST_PROGRAM_ALLOWLIST_EXTRA, []);
const hostProgramAllowlist = mergeHostProgramAllowlist({
  defaults: hostProgramDefaults,
  configured: hostProgramConfigured,
  extra: hostProgramExtra,
  replace: parseBoolean(process.env.HOST_PROGRAM_ALLOWLIST_REPLACE, false),
});
const defaultDevboxProgramAllowlist = runtimeMode === "host"
  ? hostProgramAllowlist
  : ["bash", "sh", "git", "gh", "node", "npm", "npx", "python", "python3", "pip", "pip3", "rg", "curl"];
const devboxProgramAllowlist = parseCsv(process.env.DEVBOX_PROGRAM_ALLOWLIST, defaultDevboxProgramAllowlist);
const hostSearchBackend = (process.env.HOST_SEARCH_BACKEND?.trim().toLowerCase() || "auto");
const defaultGatewayBridgeOrigins = "https://chatgpt.com,https://chat.openai.com";
const gatewayBridgeOrigins = parseCsv(process.env.GATEWAY_BRIDGE_ORIGINS ?? defaultGatewayBridgeOrigins);
const defaultDevboxUser = runtimeMode === "host" ? process.env.USER?.trim() || process.env.LOGNAME?.trim() || "" : "root";
const defaultDevboxContainerName = process.env.DEVBOX_CONTAINER_NAME?.trim() || "chatgpt-devbox-runtime";
const defaultDevboxTmpVolumeName = process.env.DEVBOX_TMP_VOLUME_NAME?.trim() || `${defaultDevboxContainerName}-tmp`;

export const config = {
  platform,
  runtimeMode,
  hostShell,
  powerShellExe,
  powerShellFallbackExe,
  legacyWindowsPowerShellExe,
  installedPowerShell7Exe,
  host: process.env.HOST?.trim() || "0.0.0.0",
  port: parseInteger(process.env.PORT, 8100),
  authMode: process.env.MCP_AUTH_MODE?.trim() || "none",
  publicBaseUrl,
  maxTextOutputChars: parseCharacterLimit(process.env.MAX_TEXT_OUTPUT_CHARS, 4000000),
  maxCommandOutputChars: parseCommandOutputLimit(process.env.MAX_COMMAND_OUTPUT_CHARS),
  maxMcpTransferChars: parseMcpTransferLimit(process.env.MAX_MCP_TRANSFER_CHARS),
  mcpJsonBodyLimit: parseJsonBodyLimit(process.env.MCP_JSON_BODY_LIMIT, "16mb"),
  mcpUsageLogMaxBytes: parseInteger(process.env.MCP_USAGE_LOG_MAX_BYTES, 16 * 1024 * 1024),
  mcpUsageLogRotations: parseInteger(process.env.MCP_USAGE_LOG_ROTATIONS, 3),
  mcpExecMaxConcurrent: parseInteger(process.env.MCP_EXEC_MAX_CONCURRENT, 6),
  mcpExecReservedInteractive: parseInteger(process.env.MCP_EXEC_RESERVED_INTERACTIVE, 1),
  mcpExecQueueTimeoutMs: parseInteger(process.env.MCP_EXEC_QUEUE_TIMEOUT_MS, 15000),
  mcpBackgroundQueueTimeoutMs: parseInteger(process.env.MCP_BACKGROUND_QUEUE_TIMEOUT_MS, 300000),
  mcpWatchMaxConcurrent: parseInteger(process.env.MCP_WATCH_MAX_CONCURRENT, 4),
  mcpExecHeavyCapacity: parseInteger(process.env.MCP_EXEC_HEAVY_CAPACITY, 4),
  mcpExecHeavyWeight: parseInteger(process.env.MCP_EXEC_HEAVY_WEIGHT, 2),
  mcpJobLogMaxBytes: parseInteger(process.env.MCP_JOB_LOG_MAX_BYTES, 32 * 1024 * 1024),
  mcpJobLogRotations: parseInteger(process.env.MCP_JOB_LOG_ROTATIONS, 2),
  mcpJobHeartbeatMs: parseInteger(process.env.MCP_JOB_HEARTBEAT_MS, 5000),
  mcpJobOrphanStaleMs: parseInteger(process.env.MCP_JOB_ORPHAN_STALE_MS, 15000),
  mcpJobRetentionHours: parseInteger(process.env.MCP_JOB_RETENTION_HOURS, 168),
  mcpJobStoreMaxBytes: parseInteger(process.env.MCP_JOB_STORE_MAX_BYTES, 2 * 1024 * 1024 * 1024),
  mcpJobStoreMaxTerminalJobs: parseInteger(process.env.MCP_JOB_STORE_MAX_TERMINAL_JOBS, 5000),
  mcpWaitMaxSeconds: Math.max(1, parseInteger(process.env.MCP_WAIT_MAX_SECONDS, 300)),
  screenCaptureAttemptTimeoutMs: parseInteger(process.env.SCREEN_CAPTURE_ATTEMPT_TIMEOUT_MS, 8000),
  screenCaptureRetries: parseInteger(process.env.SCREEN_CAPTURE_RETRIES, 1),
  screenCaptureQueueTimeoutMs: parseInteger(process.env.SCREEN_CAPTURE_QUEUE_TIMEOUT_MS, 5000),
  guardianHostPressureSampleMs: parseInteger(process.env.GUARDIAN_HOST_PRESSURE_SAMPLE_MS, 60000),
  devboxVersionCacheMs: parseInteger(process.env.DEVBOX_VERSION_CACHE_MS, 120000),
  hostSearchBackend,
  dockerCommandTimeoutMs: parseInteger(process.env.DOCKER_COMMAND_TIMEOUT_MS, 120000),
  devboxContainerName: defaultDevboxContainerName,
  devboxImageName: process.env.DEVBOX_IMAGE_NAME?.trim() || "chatgpt-devbox-runtime:local",
  devboxWorkspacePath: process.env.DEVBOX_WORKSPACE_PATH?.trim() || defaultDevboxWorkspacePath,
  hostWorkspacePath,
  devboxDefaultUser: process.env.DEVBOX_DEFAULT_USER?.trim() || defaultDevboxUser,
  devboxTmpVolumeName: defaultDevboxTmpVolumeName,
  devboxRetiredContainerGraceMs: parseInteger(process.env.DEVBOX_RETIRED_CONTAINER_GRACE_MS, 300000),
  devboxAutoStart: parseBoolean(process.env.DEVBOX_AUTO_START, true),
  enableGatewayBridge: parseBoolean(process.env.ENABLE_GATEWAY_BRIDGE, true),
  gatewayBridgeOrigins,
  enableHostExec,
  enableWindowsHostExec: enableHostExec,
  hostDefaultWorkdir: defaultHostWorkdir,
  hostProgramAllowlist,
  devboxProgramAllowlist,
  nodeExe: process.env.NODE_EXE?.trim() || defaultNodeExe,
  oauthStateFilePath: process.env.OAUTH_STATE_FILE_PATH?.trim() || defaultOauthStateFilePath,
  cloudflaredContainerName: process.env.CLOUDFLARED_CONTAINER_NAME?.trim() || "chatgpt-devbox-cloudflared",
  cloudflareAccessTeamDomain: normalizeUrl(process.env.CLOUDFLARE_ACCESS_TEAM_DOMAIN),
  cloudflareAccessAud: process.env.CLOUDFLARE_ACCESS_AUD?.trim() || "",
  cloudflareAccessJwksUrl: normalizeUrl(process.env.CLOUDFLARE_ACCESS_JWKS_URL),
};


if (!["auto", "rg", "js"].includes(config.hostSearchBackend)) {
  throw new Error(`Unsupported HOST_SEARCH_BACKEND "${config.hostSearchBackend}". Use "auto", "rg", or "js".`);
}

if (!["none", "demo-oauth", "cloudflare-access"].includes(config.authMode)) {
  throw new Error(`Unsupported MCP_AUTH_MODE "${config.authMode}". Use "none", "demo-oauth", or "cloudflare-access".`);
}

if (["demo-oauth", "cloudflare-access"].includes(config.authMode) && !config.publicBaseUrl) {
  throw new Error("PUBLIC_BASE_URL is required when MCP_AUTH_MODE uses OAuth.");
}

if (config.authMode === "cloudflare-access") {
  if (!config.cloudflareAccessTeamDomain) {
    throw new Error("CLOUDFLARE_ACCESS_TEAM_DOMAIN is required when MCP_AUTH_MODE=cloudflare-access.");
  }

  if (!config.cloudflareAccessAud) {
    throw new Error("CLOUDFLARE_ACCESS_AUD is required when MCP_AUTH_MODE=cloudflare-access.");
  }
}

export const version = "0.1.0";
