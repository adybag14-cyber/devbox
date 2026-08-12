const normalizeMode = (value) => String(value ?? "").trim().toLowerCase();
const shellBasename = (value) => String(value ?? "").trim().split(/[\\/]/).pop()?.toLowerCase() || "";

export const detectPlatform = (env = process.env, processPlatform = process.platform) => {
  const prefix = String(env.PREFIX ?? "");
  const isWindows = processPlatform === "win32";
  const isMacOS = processPlatform === "darwin";
  const isLinux = processPlatform === "linux";
  const isAndroid = processPlatform === "android";
  // Node.js built for Termux reports process.platform === "android", while some
  // emulated/test environments report "linux". PREFIX/TERMUX_VERSION are the
  // authoritative Termux signals, so accept either POSIX platform value.
  const isTermux = (isLinux || isAndroid) && Boolean(env.TERMUX_VERSION || prefix.includes("com.termux/files/usr"));
  const id = isTermux ? "termux" : isWindows ? "windows" : isMacOS ? "macos" : isLinux ? "linux" : isAndroid ? "android" : processPlatform || "unknown";
  const displayName = isTermux ? "Termux" : isWindows ? "Windows" : isMacOS ? "macOS" : isLinux ? "Linux" : isAndroid ? "Android" : processPlatform || "Unknown";

  return {
    id,
    displayName,
    isTermux,
    isWindows,
    isMacOS,
    isLinux,
    isAndroid,
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
    ? ["powershell", "pwsh", "cmd", "git", "gh", "docker", "node", "npm", "npx", "python", "py", "pip", "rg", "curl", "winget"]
    : ["bash", "sh", "git", "gh", "node", "npm", "npx", "python", "python3", "pip", "pip3", "rg", "curl"];

export const mergeHostProgramAllowlist = ({ defaults = [], configured = [], extra = [], replace = false } = {}) => {
  const normalize = (items) => items.map((value) => String(value).trim().toLowerCase()).filter(Boolean);
  const base = replace && configured.length > 0 ? normalize(configured) : normalize(defaults);
  const additions = replace ? [] : [...normalize(configured), ...normalize(extra)];
  return [...new Set([...base, ...additions])];
};

export const resolveHostShell = (env = process.env, platform = detectPlatform(env)) => {
  if (platform?.isWindows) {
    return String(env.HOST_SHELL ?? env.POWERSHELL_EXE ?? "powershell.exe").trim() || "powershell.exe";
  }

  return String(env.HOST_SHELL ?? env.SHELL ?? "/bin/sh").trim() || "/bin/sh";
};

export const buildHostShellArgs = (shell, command, platform = detectPlatform()) => {
  const name = shellBasename(shell).replace(/\.exe$/i, "");

  if (platform?.isWindows) {
    if (name === "powershell" || name === "pwsh") {
      return ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command];
    }

    if (name === "cmd") {
      return ["/d", "/s", "/c", command];
    }
  }

  return ["-lc", command];
};
