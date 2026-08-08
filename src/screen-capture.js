import { config } from "./config.js";
import { HostCommandError } from "./host-tools.js";
import { captureLinuxDisplay, captureLinuxProgram } from "./linux-screen-capture.js";
import { captureMacOSDisplay, captureMacOSProgram } from "./macos-screen-capture.js";
import { captureFullDisplayJpeg, captureProgramWindowJpeg } from "./windows-screen-capture.js";
import { withCaptureWorker } from "./capture-queue.js";
import { abortableSleep } from "./wait-utils.js";

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

const isTransientCaptureFailure = (error) =>
  Boolean(error?.timedOut)
  || /timed out|temporar|worker remained busy|resource busy|sharing violation/iu.test(String(error?.message ?? ""));

const withCapturePolicy = async ({ signal, timeoutMs }, operation) =>
  withCaptureWorker({ timeoutMs: config.screenCaptureQueueTimeoutMs, signal }, async (lease) => {
    const requestedTimeout = Number(timeoutMs);
    const attemptTimeoutMs = Math.max(
      1000,
      Math.min(
        Number.isFinite(requestedTimeout) && requestedTimeout > 0 ? requestedTimeout : config.screenCaptureAttemptTimeoutMs,
        config.screenCaptureAttemptTimeoutMs,
      ),
    );
    const maxAttempts = Math.max(1, 1 + Math.max(0, config.screenCaptureRetries));
    let lastError = null;
    for (let attempt = 1; attempt <= maxAttempts; attempt += 1) {
      try {
        const capture = await operation({ attempt, timeoutMs: attemptTimeoutMs });
        capture.metadata = {
          ...capture.metadata,
          capture_attempts: attempt,
          capture_retried: attempt > 1,
          capture_queue_wait_ms: lease.queueWaitMs,
          capture_attempt_timeout_ms: attemptTimeoutMs,
        };
        return capture;
      } catch (error) {
        lastError = error;
        if (attempt >= maxAttempts || !isTransientCaptureFailure(error)) throw error;
        await abortableSleep(150, signal);
      }
    }
    throw lastError;
  });

export const captureHostDisplay = async (options = {}) => {
  const backend = resolveScreenCaptureBackend();
  if (backend === "termux-unsupported") {
    throw new HostCommandError(
      "Termux/Android does not expose a desktop screenshot API to ordinary terminal apps. Use Android's platform screenshot/MediaProjection APIs outside Devbox or capture the desktop from the host running the emulator.",
    );
  }
  if (backend === "unsupported") {
    throw new HostCommandError(`Screen capture is not supported on host platform ${config.platform.displayName}.`);
  }

  return withCapturePolicy(options, async ({ timeoutMs }) => {
    switch (backend) {
      case "windows":
        return normalizeWindowsCapture(await captureFullDisplayJpeg({ ...options, timeoutMs }));
      case "macos":
        return captureMacOSDisplay({ ...options, timeoutMs });
      case "linux":
        return captureLinuxDisplay({ ...options, timeoutMs });
      default:
        throw new HostCommandError(`Screen capture is not supported on host platform ${config.platform.displayName}.`);
    }
  });
};

export const captureHostProgram = async ({ pid, quality = 85, timeoutMs, includeProcessTree = true, signal } = {}) => {
  if (!Number.isInteger(pid) || pid <= 0) throw new HostCommandError("pid must be a positive host process ID.");
  const backend = resolveScreenCaptureBackend();
  if (backend === "termux-unsupported") {
    throw new HostCommandError(
      "Termux/Android cannot capture another app's window by PID from a terminal process. Android requires MediaProjection/user consent for cross-app screen capture.",
    );
  }
  if (backend === "unsupported") {
    throw new HostCommandError(`Program-window capture is not supported on host platform ${config.platform.displayName}.`);
  }

  return withCapturePolicy({ signal, timeoutMs }, async ({ timeoutMs: attemptTimeoutMs }) => {
    const options = { pid, quality, timeoutMs: attemptTimeoutMs, includeProcessTree, signal };
    switch (backend) {
      case "windows":
        return normalizeWindowsCapture(await captureProgramWindowJpeg(options));
      case "macos":
        return captureMacOSProgram(options);
      case "linux":
        return captureLinuxProgram(options);
      default:
        throw new HostCommandError(`Program-window capture is not supported on host platform ${config.platform.displayName}.`);
    }
  });
};

export const screenCaptureInternals = { isTransientCaptureFailure, withCapturePolicy };
