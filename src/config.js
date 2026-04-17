import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

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

const rawPublicBaseUrl = process.env.PUBLIC_BASE_URL?.trim();
const publicBaseUrl = rawPublicBaseUrl ? rawPublicBaseUrl.replace(/\/+$/, "") : "";
const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const defaultHostWorkspacePath = path.join(projectRoot, "workspace");
const defaultOauthStateFilePath = path.join(projectRoot, "run", "oauth-state.json");
const defaultHostWorkdir = os.homedir() || projectRoot;
const defaultNodeExe = process.execPath || "node";
const defaultDevboxContainerName = process.env.DEVBOX_CONTAINER_NAME?.trim() || "chatgpt-devbox-runtime";
const defaultDevboxTmpVolumeName = process.env.DEVBOX_TMP_VOLUME_NAME?.trim() || `${defaultDevboxContainerName}-tmp`;

export const config = {
  host: process.env.HOST?.trim() || "0.0.0.0",
  port: parseInteger(process.env.PORT, 8100),
  authMode: process.env.MCP_AUTH_MODE?.trim() || "demo-oauth",
  publicBaseUrl,
  maxTextOutputChars: parseInteger(process.env.MAX_TEXT_OUTPUT_CHARS, 20000),
  devboxContainerName: defaultDevboxContainerName,
  devboxImageName: process.env.DEVBOX_IMAGE_NAME?.trim() || "chatgpt-devbox-runtime:local",
  devboxWorkspacePath: process.env.DEVBOX_WORKSPACE_PATH?.trim() || "/workspace",
  hostWorkspacePath: process.env.HOST_WORKSPACE_PATH?.trim() || defaultHostWorkspacePath,
  devboxDefaultUser: process.env.DEVBOX_DEFAULT_USER?.trim() || "root",
  devboxTmpVolumeName: defaultDevboxTmpVolumeName,
  devboxRetiredContainerGraceMs: parseInteger(process.env.DEVBOX_RETIRED_CONTAINER_GRACE_MS, 300000),
  devboxAutoStart: parseBoolean(process.env.DEVBOX_AUTO_START, true),
  enableWindowsHostExec: parseBoolean(process.env.ENABLE_WINDOWS_HOST_EXEC, true),
  hostDefaultWorkdir: process.env.HOST_DEFAULT_WORKDIR?.trim() || defaultHostWorkdir,
  hostProgramAllowlist: parseCsv(
    process.env.HOST_PROGRAM_ALLOWLIST,
    ["powershell", "pwsh", "cmd", "git", "gh", "docker", "node", "npm", "npx", "python", "py", "pip", "winget"],
  ),
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
