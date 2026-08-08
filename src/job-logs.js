import {
  closeSync,
  existsSync,
  fstatSync,
  openSync,
  renameSync,
  rmSync,
  statSync,
  writeSync,
} from "node:fs";

const safeSize = (filePath) => {
  try {
    return statSync(filePath).size;
  } catch {
    return 0;
  }
};

const rotateFiles = (filePath, rotations) => {
  const count = Math.max(0, Number(rotations) || 0);
  if (count <= 0) {
    rmSync(filePath, { force: true });
    return;
  }
  rmSync(`${filePath}.${count}`, { force: true });
  for (let index = count - 1; index >= 1; index -= 1) {
    const source = `${filePath}.${index}`;
    if (existsSync(source)) renameSync(source, `${filePath}.${index + 1}`);
  }
  if (existsSync(filePath)) renameSync(filePath, `${filePath}.1`);
};

export const createRotatingFileSink = (filePath, {
  maxBytes = 32 * 1024 * 1024,
  rotations = 2,
} = {}) => {
  const limit = Math.max(4096, Number(maxBytes) || 32 * 1024 * 1024);
  const rotationCount = Math.max(0, Number(rotations) || 0);
  let fd = openSync(filePath, "a");
  let currentBytes = fstatSync(fd).size;
  let totalBytes = 0;
  let rotationsPerformed = 0;
  let closed = false;
  let failure = null;

  const closeCurrent = () => {
    if (fd === null) return;
    const current = fd;
    fd = null;
    closeSync(current);
  };

  const reopen = () => {
    const nextFd = openSync(filePath, "a");
    try {
      currentBytes = fstatSync(nextFd).size;
      fd = nextFd;
    } catch (error) {
      closeSync(nextFd);
      throw error;
    }
  };

  const rotate = () => {
    closeCurrent();
    try {
      rotateFiles(filePath, rotationCount);
      reopen();
      rotationsPerformed += 1;
    } catch (error) {
      closed = true;
      failure = error;
      throw error;
    }
  };

  return {
    write(value) {
      if (failure) throw failure;
      if (closed || value === undefined || value === null || value === "") return;
      if (fd === null) throw new Error(`Rotating log sink for ${filePath} has no writable file descriptor.`);
      let buffer = Buffer.isBuffer(value) ? value : Buffer.from(String(value));
      totalBytes += buffer.length;
      while (buffer.length > 0) {
        if (currentBytes >= limit) rotate();
        if (fd === null) throw new Error(`Rotating log sink for ${filePath} became unavailable during rotation.`);
        const available = Math.max(1, limit - currentBytes);
        const slice = buffer.subarray(0, Math.min(available, buffer.length));
        try {
          writeSync(fd, slice);
        } catch (error) {
          failure = error;
          closed = true;
          try { closeCurrent(); } catch {}
          throw error;
        }
        currentBytes += slice.length;
        buffer = buffer.subarray(slice.length);
      }
    },
    end() {
      if (closed && fd === null) return;
      closed = true;
      closeCurrent();
    },
    snapshot() {
      return {
        maxBytes: limit,
        rotations: rotationCount,
        rotationsPerformed,
        totalBytes,
        currentBytes: fd === null ? safeSize(filePath) : currentBytes,
        truncated: rotationsPerformed > 0,
        failed: failure !== null,
        error: failure instanceof Error ? failure.message : failure ? String(failure) : null,
      };
    },
  };
};

export const jobLogInternals = { rotateFiles, safeSize };
