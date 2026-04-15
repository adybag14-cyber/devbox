const normalizeMode = (value) => String(value ?? "").trim().toLowerCase();

export const detectPlatform = (env = process.env, processPlatform = process.platform) => {
  const prefix = String(env.PREFIX ?? "");
  const isTermux = Boolean(env.TERMUX_VERSION || prefix.includes("com.termux/files/usr"));
  const isWindows = processPlatform === "win32";
  const isMacOS = processPlatform === "darwin";
  const isLinux = processPlatform === "linux" || isTermux;
  const id = isTermux ? "termux" : isWindows ? "windows" : isMacOS ? "macos" : isLinux ? "linux" : processPlatform || "unknown";
  const displayName = isTermux ? "Termux" : isWindows ? "Windows" : isMacOS ? "macOS" : isLinux ? "Linux" : processPlatform || "Unknown";

  return {
    id,
    displayName,
    isTermux,
    isWindows,
    isMacOS,
    isLinux,
    nodePlatform: processPlatform,
  };
};

export const resolveRuntimeMode = ({ requestedMode, platform }) => {
  const normalizedMode = normalizeMode(requestedMode);
  if (normalizedMode === "host" || normalizedMode === "docker") {
    return normalizedMode;
  }

  if (platform?.isWindows) {
    return "docker";
  }

  return "host";
};

export const defaultHostProgramAllowlist = (platform) =>
  platform?.isWindows
    ? ["powershell", "pwsh", "cmd", "git", "gh", "docker", "node", "npm", "npx", "python", "py", "pip", "winget"]
    : ["bash", "sh", "git", "gh", "node", "npm", "npx", "python", "python3", "pip", "pip3", "rg", "curl"];

export const resolveHostShell = (env = process.env, platform = detectPlatform(env)) => {
  if (platform?.isWindows) {
    return String(env.HOST_SHELL ?? env.ComSpec ?? "powershell.exe").trim() || "powershell.exe";
  }

  return String(env.HOST_SHELL ?? env.SHELL ?? "/bin/sh").trim() || "/bin/sh";
};
