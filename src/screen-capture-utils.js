import { createHash } from "node:crypto";
import { constants as fsConstants } from "node:fs";
import { access } from "node:fs/promises";
import path from "node:path";

import { HostCommandError } from "./host-tools.js";
import { spawnProcess } from "./process-utils.js";

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

export const isPngBuffer = (buffer) =>
  Buffer.isBuffer(buffer) && buffer.length >= 24 && buffer.subarray(0, PNG_SIGNATURE.length).equals(PNG_SIGNATURE);

export const getPngDimensions = (buffer) => {
  if (!isPngBuffer(buffer) || buffer.toString("ascii", 12, 16) !== "IHDR") {
    return null;
  }
  return { width: buffer.readUInt32BE(16), height: buffer.readUInt32BE(20) };
};

export const finalizeImageCapture = ({ image, mimeType, metadata = {} }) => {
  if (!Buffer.isBuffer(image) || image.length === 0) {
    throw new HostCommandError("Screen capture did not return image bytes.");
  }
  const dimensions = mimeType === "image/png" ? getPngDimensions(image) : null;
  return {
    image,
    mimeType,
    metadata: {
      ...metadata,
      ...(dimensions ?? {}),
      mime_type: mimeType,
      bytes: image.length,
      sha256: createHash("sha256").update(image).digest("hex"),
    },
  };
};

export const findExecutable = async (names, env = process.env) => {
  const pathValue = String(env.PATH ?? "");
  const directories = pathValue.split(path.delimiter).filter(Boolean);
  for (const name of names) {
    if (path.isAbsolute(name)) {
      try {
        await access(name, fsConstants.X_OK);
        return name;
      } catch {
        continue;
      }
    }
    for (const directory of directories) {
      const candidate = path.join(directory, name);
      try {
        await access(candidate, fsConstants.X_OK);
        return candidate;
      } catch {
      }
    }
  }
  return null;
};

export const parsePsProcessTable = (text) => {
  const rows = [];
  for (const line of String(text ?? "").split(/\r?\n/u)) {
    const match = line.match(/^\s*(\d+)\s+(\d+)\s*$/u);
    if (!match) continue;
    rows.push({ pid: Number.parseInt(match[1], 10), ppid: Number.parseInt(match[2], 10) });
  }
  return rows;
};

export const collectProcessTreePids = (rows, rootPid) => {
  const root = Number(rootPid);
  const seen = new Set([root]);
  const queue = [root];
  while (queue.length > 0) {
    const parent = queue.shift();
    for (const row of rows) {
      if (row.ppid === parent && !seen.has(row.pid)) {
        seen.add(row.pid);
        queue.push(row.pid);
      }
    }
  }
  return [...seen];
};

export const getPosixProcessTreePids = async (rootPid, { timeoutMs = 5000 } = {}) => {
  try {
    const result = await spawnProcess("ps", ["-axo", "pid=,ppid="], { timeoutMs, maxCaptureChars: 2_000_000 });
    return collectProcessTreePids(parsePsProcessTable(result.stdout), rootPid);
  } catch {
    return [rootPid];
  }
};

export const captureCommand = async (file, args, { cwd, timeoutMs = 30000, env = process.env } = {}) => {
  try {
    return await spawnProcess(file, args, { cwd, timeoutMs, env, maxCaptureChars: 200_000 });
  } catch (error) {
    throw new HostCommandError(error instanceof Error ? error.message : `Failed to run ${file}.`, {
      exitCode: error?.exitCode,
      stdout: error?.stdout,
      stderr: error?.stderr,
      data: { file, args },
    });
  }
};
