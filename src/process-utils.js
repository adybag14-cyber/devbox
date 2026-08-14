import { spawn } from "node:child_process";
import { StringDecoder } from "node:string_decoder";

export const MAX_PROCESS_ERROR_MESSAGE_CHARS = 4096;

export const summarizeProcessFailure = ({ file, code, stdout = "", stderr = "" }) => {
  const trimmedStderr = String(stderr).trim();
  if (trimmedStderr) {
    if (trimmedStderr.length <= MAX_PROCESS_ERROR_MESSAGE_CHARS) {
      return trimmedStderr;
    }
    const suffix = `\n... error summary truncated to ${MAX_PROCESS_ERROR_MESSAGE_CHARS} characters ...`;
    return trimmedStderr.slice(0, Math.max(0, MAX_PROCESS_ERROR_MESSAGE_CHARS - suffix.length)) + suffix;
  }

  const stdoutLength = String(stdout).length;
  return stdoutLength > 0
    ? `${file} exited with code ${code} after producing ${stdoutLength} characters of stdout; see the bounded stdout field.`
    : `${file} exited with code ${code}.`;
};

export class SpawnProcessError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "SpawnProcessError";
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
    this.file = details.file ?? "";
    this.args = details.args ?? [];
    this.timedOut = details.timedOut === true;
    this.aborted = details.aborted === true;
    this.signal = details.signal ?? null;
    this.elapsedMs = details.elapsedMs ?? null;
  }
}

const killProcessTree = (child) => {
  if (!child.pid) {
    return;
  }

  if (process.platform === "win32") {
    const killer = spawn("taskkill.exe", ["/pid", String(child.pid), "/t", "/f"], {
      stdio: "ignore",
      windowsHide: true,
    });
    killer.on("error", () => {});
    killer.unref();
    return;
  }

  child.kill("SIGTERM");
  const forceKillTimer = setTimeout(() => {
    try {
      child.kill("SIGKILL");
    } catch {
    }
  }, 1000);
  forceKillTimer.unref();
};

export const spawnProcess = (file, args, options = {}) =>
  new Promise((resolve, reject) => {
    const startedAtMs = Date.now();
    const captureLimit = Number.isFinite(options.maxCaptureChars) ? Math.max(0, options.maxCaptureChars) : null;
    const createCapture = () => ({ head: "", tail: "", originalChars: 0, truncated: false });
    const codePointLength = (value) => { let count = 0; for (const _ of String(value ?? "")) count += 1; return count; };
    const takeCodePoints = (value, limit) => Array.from(String(value ?? "")).slice(0, Math.max(0, limit)).join("");
    const tailCodePoints = (value, limit) => limit <= 0 ? "" : Array.from(String(value ?? "")).slice(-limit).join("");
    const stdoutCapture = createCapture();
    const stderrCapture = createCapture();
    const stdoutDecoder = new StringDecoder("utf8");
    const stderrDecoder = new StringDecoder("utf8");
    const appendCapture = (state, text) => {
      const value = String(text ?? "");
      state.originalChars += codePointLength(value);
      if (captureLimit === null) { state.head += value; return; }
      if (captureLimit === 0) { state.truncated ||= codePointLength(value) > 0; return; }
      if (!state.truncated && state.originalChars <= captureLimit) { state.head += value; return; }
      const headLimit = Math.floor(captureLimit / 2);
      const tailLimit = captureLimit - headLimit;
      if (!state.truncated) {
        const combined = state.head + value;
        state.head = takeCodePoints(combined, headLimit);
        state.tail = tailCodePoints(combined, tailLimit);
        state.truncated = true;
      } else {
        state.tail = tailCodePoints(state.tail + value, tailLimit);
      }
    };
    const captureText = (state) => {
      if (!state.truncated) return state.head;
      if (captureLimit === 0) return "";
      const omitted = Math.max(0, state.originalChars - codePointLength(state.head) - codePointLength(state.tail));
      return `${state.head}\n... middle capture omitted ${omitted} characters ...\n${state.tail}`;
    };
    const captureDetails = () => ({
      stdout: captureText(stdoutCapture),
      stderr: captureText(stderrCapture),
      stdoutOriginalChars: stdoutCapture.originalChars,
      stderrOriginalChars: stderrCapture.originalChars,
      stdoutCaptureTruncated: stdoutCapture.truncated,
      stderrCaptureTruncated: stderrCapture.truncated,
    });
    let stdout = "";
    let stderr = "";
    let timedOut = false;
    let aborted = false;
    let settled = false;
    let timer = null;
    let timeoutRejectTimer = null;

    const buildTimeoutError = (code = null, signal = null) =>
      new SpawnProcessError(`Command timed out after ${options.timeoutMs} ms.`, {
        exitCode: code,
        stdout,
        stderr,
        file,
        args,
        timedOut: true,
        signal,
        elapsedMs: Date.now() - startedAtMs,
      });
    const buildAbortError = (code = null, signal = null) =>
      new SpawnProcessError("Command cancelled by the MCP client.", {
        exitCode: code,
        stdout,
        stderr,
        file,
        args,
        aborted: true,
        signal,
        elapsedMs: Date.now() - startedAtMs,
      });

    const cleanupTimers = () => {
      if (timer) {
        clearTimeout(timer);
        timer = null;
      }

      if (timeoutRejectTimer) {
        clearTimeout(timeoutRejectTimer);
        timeoutRejectTimer = null;
      }
      options.signal?.removeEventListener("abort", handleAbort);
    };

    const settleResolve = (value) => {
      if (settled) {
        return;
      }

      settled = true;
      cleanupTimers();
      resolve(value);
    };

    const settleReject = (error) => {
      if (settled) {
        return;
      }

      settled = true;
      cleanupTimers();
      reject(error);
    };

    const child = spawn(file, args, {
      cwd: options.cwd,
      env: options.env ?? process.env,
      shell: false,
      windowsHide: true,
    });
    try { options.onSpawn?.(child.pid ?? null); } catch {}

    const handleAbort = () => {
      if (settled || aborted) {
        return;
      }
      aborted = true;
      killProcessTree(child);
      timeoutRejectTimer = setTimeout(() => {
        settleReject(buildAbortError(null));
      }, options.timeoutRejectGraceMs ?? 3000);
      timeoutRejectTimer.unref?.();
    };

    if (options.signal?.aborted) {
      handleAbort();
    } else {
      options.signal?.addEventListener("abort", handleAbort, { once: true });
    }

    if (options.timeoutMs) {
      timer = setTimeout(() => {
        timedOut = true;
        killProcessTree(child);
        timeoutRejectTimer = setTimeout(() => {
          settleReject(buildTimeoutError(null));
        }, options.timeoutRejectGraceMs ?? 3000);
        timeoutRejectTimer.unref?.();
      }, options.timeoutMs);
      timer.unref?.();
    }

    const pushStdout = (text) => {
      if (!text) return;
      appendCapture(stdoutCapture, text);
      stdout = captureText(stdoutCapture);
      try { options.onStdout?.(text); } catch {}
    };
    const pushStderr = (text) => {
      if (!text) return;
      appendCapture(stderrCapture, text);
      stderr = captureText(stderrCapture);
      try { options.onStderr?.(text); } catch {}
    };

    child.stdout.on("data", (chunk) => pushStdout(stdoutDecoder.write(chunk)));
    child.stderr.on("data", (chunk) => pushStderr(stderrDecoder.write(chunk)));

    child.on("error", (error) => {
      settleReject(
        new SpawnProcessError(error.message, {
          stdout,
          stderr,
          file,
          args,
        }),
      );
    });

    child.on("close", (code, signal) => {
      pushStdout(stdoutDecoder.end());
      pushStderr(stderrDecoder.end());
      if (timedOut) {
        settleReject(buildTimeoutError(code, signal));
        return;
      }

      if (aborted) {
        settleReject(buildAbortError(code, signal));
        return;
      }

      if (code !== 0) {
        settleReject(
          new SpawnProcessError(summarizeProcessFailure({ file, code, stdout, stderr }), {
            exitCode: code,
            stdout,
            stderr,
            file,
            args,
          }),
        );
        return;
      }

      settleResolve({
        ...captureDetails(),
        exitCode: code,
      });
    });

    if (options.input !== undefined) {
      child.stdin.end(options.input);
    } else {
      child.stdin.end();
    }
  });

export const trimText = (text, maxChars) => {
  if (!text) {
    return { text: "", truncated: false };
  }

  if (maxChars === null || maxChars === undefined || !Number.isFinite(maxChars)) {
    return { text, truncated: false };
  }

  if (text.length <= maxChars) {
    return { text, truncated: false };
  }

  const suffix = `\n... truncated to ${maxChars} characters ...`;
  return {
    text: text.slice(0, Math.max(0, maxChars - suffix.length)) + suffix,
    truncated: true,
  };
};
