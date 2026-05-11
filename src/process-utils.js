import { spawn } from "node:child_process";

export class SpawnProcessError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "SpawnProcessError";
    this.exitCode = details.exitCode ?? null;
    this.stdout = details.stdout ?? "";
    this.stderr = details.stderr ?? "";
    this.file = details.file ?? "";
    this.args = details.args ?? [];
  }
}

export const spawnProcess = (file, args, options = {}) =>
  new Promise((resolve, reject) => {
    let stdout = "";
    let stderr = "";
    let timedOut = false;

    const child = spawn(file, args, {
      cwd: options.cwd,
      env: options.env ?? process.env,
      shell: false,
      windowsHide: true,
    });

    let timer = null;
    if (options.timeoutMs) {
      timer = setTimeout(() => {
        timedOut = true;
        child.kill();
      }, options.timeoutMs);
    }

    child.stdout.on("data", (chunk) => {
      stdout += chunk.toString();
    });

    child.stderr.on("data", (chunk) => {
      stderr += chunk.toString();
    });

    child.on("error", (error) => {
      if (timer) {
        clearTimeout(timer);
      }

      reject(
        new SpawnProcessError(error.message, {
          stdout,
          stderr,
          file,
          args,
        }),
      );
    });

    child.on("close", (code) => {
      if (timer) {
        clearTimeout(timer);
      }

      if (timedOut) {
        reject(
          new SpawnProcessError(`Command timed out after ${options.timeoutMs} ms.`, {
            exitCode: code,
            stdout,
            stderr,
            file,
            args,
          }),
        );
        return;
      }

      if (code !== 0) {
        reject(
          new SpawnProcessError(stderr.trim() || stdout.trim() || `${file} exited with code ${code}.`, {
            exitCode: code,
            stdout,
            stderr,
            file,
            args,
          }),
        );
        return;
      }

      resolve({
        stdout,
        stderr,
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
