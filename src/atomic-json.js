import { randomUUID } from "node:crypto";
import { open, rename, rm } from "node:fs/promises";
import path from "node:path";
import { setTimeout as sleep } from "node:timers/promises";

const writes = new Map();
const retryableWindowsErrors = new Set(["EACCES", "EBUSY", "EPERM", "EEXIST"]);

const replaceText = async (filePath, payload) => {
  const temporary = `${filePath}.${process.pid}.${randomUUID()}.tmp`;
  // Exclusive creation makes cleanup apply only to our own temporary file.
  const handle = await open(temporary, "wx", 0o600);
  try {
    try {
      await handle.writeFile(payload, "utf8");
    } finally {
      await handle.close();
    }
    for (let attempt = 0; ; attempt += 1) {
      try {
        await rename(temporary, filePath);
        return;
      } catch (error) {
        if (process.platform !== "win32" || !retryableWindowsErrors.has(error?.code) || attempt >= 9) throw error;
        // Ten attempts, at most 555 ms of backoff. A persistent error still fails.
        await sleep(Math.min(100, 5 * 2 ** attempt));
      }
    }
  } finally {
    await rm(temporary, { force: true }).catch(() => {});
  }
};

export const writeJsonAtomic = async (filePath, value) => {
  const encoded = JSON.stringify(value, null, 2);
  if (encoded === undefined) throw new TypeError("Atomic JSON value must be serializable");
  const key = path.resolve(filePath);
  const previous = writes.get(key) ?? Promise.resolve();
  const current = previous.catch(() => {}).then(() => replaceText(key, `${encoded}\n`));
  writes.set(key, current);
  try {
    await current;
  } finally {
    if (writes.get(key) === current) writes.delete(key);
  }
};
