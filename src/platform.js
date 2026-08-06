const normalizeMode = (value) => String(value ?? "").trim().toLowerCase();
const shellBasename = (value) => String(value ?? "").trim().split(/[\\/]/).pop()?.toLowerCase() || "";

export const detectPlatform = (env = process.env, processPlatform = process.platform) => {
  const prefix = String(env.PREFIX ?? "");
  const isWindows = processPlatform === "win32";
  const isMacOS = processPlatform === "darwin";
  const isLinux = processPlatform === "linux";
  const isTermux = isLinux && Boolean(env.TERMUX_VERSION || prefix.includes("com.termux/files/usr"));
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
