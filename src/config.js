import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { loadEnvFile } from "./env.js";
import { defaultHostProgramAllowlist, detectPlatform, resolveHostShell, resolveRuntimeMode } from "./platform.js";

const parseInteger = (value, fallback) => {
  const parsed = Number.parseInt(value ?? "", 10);
  return Number.isFinite(parsed) ? parsed : fallback;
};

const parseBoolean = (value, fallback = false) => {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }

  const normalized = String(value).trim().toLowerCase();
  return ["1", "true", "yes", "on"].includes(normalized);
};

const parseCsv = (value, fallback = []) =>
  String(value ?? "")
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
const hostShell = resolveHostShell(process.env, platform);
const hostProgramAllowlist = parseCsv(process.env.HOST_PROGRAM_ALLOWLIST, defaultHostProgramAllowlist(platform));
const defaultGatewayBridgeOrigins = "https://chatgpt.com,https://chat.openai.com";
const gatewayBridgeOrigins = parseCsv(process.env.GATEWAY_BRIDGE_ORIGINS ?? defaultGatewayBridgeOrigins);
const defaultDevboxUser = runtimeMode === "host" ? process.env.USER?.trim() || process.env.LOGNAME?.trim() || "" : "root";

export const config = {
  platform,
  runtimeMode,
  hostShell,
  host: process.env.HOST?.trim() || "0.0.0.0",
  port: parseInteger(process.env.PORT, 8100),
  authMode: process.env.MCP_AUTH_MODE?.trim() || "none",
  publicBaseUrl,
  maxTextOutputChars: parseInteger(process.env.MAX_TEXT_OUTPUT_CHARS, 20000),
  devboxContainerName: process.env.DEVBOX_CONTAINER_NAME?.trim() || "chatgpt-devbox-runtime",
  devboxImageName: process.env.DEVBOX_IMAGE_NAME?.trim() || "chatgpt-devbox-runtime:local",
  devboxWorkspacePath: process.env.DEVBOX_WORKSPACE_PATH?.trim() || defaultDevboxWorkspacePath,
  hostWorkspacePath,
  devboxDefaultUser: process.env.DEVBOX_DEFAULT_USER?.trim() || defaultDevboxUser,
  devboxAutoStart: parseBoolean(process.env.DEVBOX_AUTO_START, true),
  enableGatewayBridge: parseBoolean(process.env.ENABLE_GATEWAY_BRIDGE, true),
  gatewayBridgeOrigins,
  enableHostExec,
  enableWindowsHostExec: enableHostExec,
  hostDefaultWorkdir: defaultHostWorkdir,
  hostProgramAllowlist,
  nodeExe: process.env.NODE_EXE?.trim() || defaultNodeExe,
  oauthStateFilePath: process.env.OAUTH_STATE_FILE_PATH?.trim() || defaultOauthStateFilePath,
  cloudflaredContainerName: process.env.CLOUDFLARED_CONTAINER_NAME?.trim() || "chatgpt-devbox-cloudflared",
  cloudflareAccessTeamDomain: normalizeUrl(process.env.CLOUDFLARE_ACCESS_TEAM_DOMAIN),
  cloudflareAccessAud: process.env.CLOUDFLARE_ACCESS_AUD?.trim() || "",
  cloudflareAccessJwksUrl: normalizeUrl(process.env.CLOUDFLARE_ACCESS_JWKS_URL),
};

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
