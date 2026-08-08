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

  const reopen = () => {
    fd = openSync(filePath, "a");
    currentBytes = fstatSync(fd).size;
  };

  const rotate = () => {
    closeSync(fd);
    rotateFiles(filePath, rotationCount);
    rotationsPerformed += 1;
    reopen();
  };

  return {
    write(value) {
      if (closed || value === undefined || value === null || value === "") return;
      let buffer = Buffer.isBuffer(value) ? value : Buffer.from(String(value));
      totalBytes += buffer.length;
      while (buffer.length > 0) {
        if (currentBytes >= limit) rotate();
        const available = Math.max(1, limit - currentBytes);
        const slice = buffer.subarray(0, Math.min(available, buffer.length));
        writeSync(fd, slice);
        currentBytes += slice.length;
        buffer = buffer.subarray(slice.length);
      }
    },
    end() {
      if (closed) return;
      closed = true;
      closeSync(fd);
    },
    snapshot() {
      return {
        maxBytes: limit,
        rotations: rotationCount,
        rotationsPerformed,
        totalBytes,
        currentBytes: closed ? safeSize(filePath) : currentBytes,
        truncated: rotationsPerformed > 0,
      };
    },
  };
};

export const jobLogInternals = { rotateFiles, safeSize };
