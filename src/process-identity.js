import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
const CACHE_TTL_MS = 1500;
const cache = new Map();
let linuxBootIdPromise = null;
let currentProcessInstancePromise = null;

export const processAlive = (pid) => {
  if (!Number.isInteger(pid) || pid < 1) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    return error?.code === "EPERM";
  }
};

const windowsProcessInstance = async (pid) => {
  const executable = String(process.env.POWERSHELL_EXE ?? "").trim() || "powershell.exe";
  const command = `$p=[System.Diagnostics.Process]::GetProcessById(${pid}); [Console]::Out.Write($p.StartTime.ToUniversalTime().ToFileTimeUtc())`;
  const { stdout } = await execFileAsync(executable, [
    "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command,
  ], { encoding: "utf8", timeout: 2500, windowsHide: true, maxBuffer: 4096 });
  const value = String(stdout ?? "").trim();
  if (!/^\d+$/u.test(value)) return null;
  return BigInt(value).toString();
};

const linuxBootId = () => {
  linuxBootIdPromise ??= readFile("/proc/sys/kernel/random/boot_id", "utf8")
    .then((value) => value.trim())
    .catch(() => "");
  return linuxBootIdPromise;
};

const linuxProcessInstance = async (pid) => {
  const [stat, bootId] = await Promise.all([
    readFile(`/proc/${pid}/stat`, "utf8"),
    linuxBootId(),
  ]);
  const close = stat.lastIndexOf(")");
  if (close < 0 || !bootId) return null;
  const fields = stat.slice(close + 1).trim().split(/\s+/u);
  const startTicks = fields[19];
  if (!startTicks) return null;
  const digest = createHash("sha256").update(`${bootId}:${startTicks}`, "utf8").digest();
  return digest.readBigUInt64LE(0).toString();
};

const macosProcessInstance = async (pid) => {
  const { stdout } = await execFileAsync("ps", ["-o", "lstart=", "-p", String(pid)], {
    encoding: "utf8", timeout: 2000, maxBuffer: 4096,
    env: { ...process.env, LC_ALL: "C" },
  });
  const millis = Date.parse(String(stdout ?? "").trim());
  return Number.isFinite(millis) ? String(Math.floor(millis / 1000)) : null;
};

export const processInstance = async (pid, { bypassCache = false } = {}) => {
  if (!Number.isInteger(pid) || pid < 1 || !processAlive(pid)) return null;
  const now = Date.now();
  if (!bypassCache) {
    const cached = cache.get(pid);
    if (cached && now - cached.sampledAtMs < CACHE_TTL_MS) return cached.value;
  }
  let value = null;
  try {
    if (process.platform === "win32") value = await windowsProcessInstance(pid);
    else if (process.platform === "linux") value = await linuxProcessInstance(pid);
    else if (process.platform === "darwin") value = await macosProcessInstance(pid);
  } catch {
    value = null;
  }
  if (!bypassCache) cache.set(pid, { sampledAtMs: now, value });
  return value;
};

export const currentProcessInstance = () => {
  currentProcessInstancePromise ??= processInstance(process.pid, { bypassCache: true }).then(value => {
    // A transient failed probe is not a permanent process identity. Share the
    // in-flight request, but let a later operation retry after an unknown result.
    if (value === null) currentProcessInstancePromise = null;
    return value;
  });
  return currentProcessInstancePromise;
};

export const processMatchesInstance = async (pid, expected) => {
  if (!processAlive(pid)) return false;
  if (expected === null || expected === undefined || expected === "") return true;
  const actual = pid === process.pid
    ? await currentProcessInstance()
    : await processInstance(pid, { bypassCache: true });
  // A failed identity probe is indeterminate, not proof of PID reuse. Verified
  // liveness remains authoritative until a successful identity probe disagrees.
  if (actual === null) return true;
  if (typeof expected === "number" && !Number.isSafeInteger(expected)) {
    // Legacy JSON numeric identities above 2^53 cannot be represented exactly in
    // JavaScript. Preserve upgrade compatibility by falling back to verified PID
    // liveness for those old records; all newly written identities are strings.
    return true;
  }
  const normalizedExpected = typeof expected === "bigint" ? expected.toString() : String(expected);
  return normalizedExpected === actual;
};

export const processIdentityInternals = { processAlive, cache };
