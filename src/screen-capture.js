import { config } from "./config.js";
import { HostCommandError } from "./host-tools.js";
import { captureLinuxDisplay, captureLinuxProgram } from "./linux-screen-capture.js";
import { captureMacOSDisplay, captureMacOSProgram } from "./macos-screen-capture.js";
import { captureFullDisplayJpeg, captureProgramWindowJpeg } from "./windows-screen-capture.js";

export const resolveScreenCaptureBackend = (platform = config.platform) => {
  if (platform?.isWindows) return "windows";
  if (platform?.isMacOS) return "macos";
  if (platform?.isLinux && !platform?.isTermux) return "linux";
  if (platform?.isTermux) return "termux-unsupported";
  return "unsupported";
};

const normalizeWindowsCapture = (capture) => ({
  image: capture.image ?? capture.jpeg,
  mimeType: capture.mimeType ?? "image/jpeg",
  metadata: capture.metadata,
});

export const captureHostDisplay = async (options = {}) => {
  switch (resolveScreenCaptureBackend()) {
    case "windows":
      return normalizeWindowsCapture(await captureFullDisplayJpeg(options));
    case "macos":
      return captureMacOSDisplay(options);
    case "linux":
      return captureLinuxDisplay(options);
    case "termux-unsupported":
      throw new HostCommandError(
        "Termux/Android does not expose a desktop screenshot API to ordinary terminal apps. Use Android's platform screenshot/MediaProjection APIs outside Devbox or capture the desktop from the host running the emulator.",
      );
    default:
      throw new HostCommandError(`Screen capture is not supported on host platform ${config.platform.displayName}.`);
  }
};

export const captureHostProgram = async ({ pid, quality = 85, timeoutMs = 30000, includeProcessTree = true } = {}) => {
  if (!Number.isInteger(pid) || pid <= 0) throw new HostCommandError("pid must be a positive host process ID.");
  const options = { pid, quality, timeoutMs, includeProcessTree };
  switch (resolveScreenCaptureBackend()) {
    case "windows":
      return normalizeWindowsCapture(await captureProgramWindowJpeg(options));
    case "macos":
      return captureMacOSProgram(options);
    case "linux":
      return captureLinuxProgram(options);
    case "termux-unsupported":
      throw new HostCommandError(
        "Termux/Android cannot capture another app's window by PID from a terminal process. Android requires MediaProjection/user consent for cross-app screen capture.",
      );
    default:
      throw new HostCommandError(`Program-window capture is not supported on host platform ${config.platform.displayName}.`);
  }
};
