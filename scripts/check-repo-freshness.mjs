import { execFile } from "node:child_process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
const args = new Set(process.argv.slice(2));
const jsonMode = args.has("--json");
const requireCurrent = args.has("--require-current");

const git = async (gitArgs) => {
  const { stdout } = await execFileAsync("git", gitArgs, {
    cwd: process.cwd(),
    windowsHide: true,
    maxBuffer: 2 * 1024 * 1024,
  });
  return stdout.trim();
};

const parseRemoteHead = (value) => String(value ?? "").trim().split(/\s+/u)[0] || null;

const head = await git(["rev-parse", "HEAD"]);
const branch = await git(["branch", "--show-current"]).catch(() => "");
const tracking = await git(["rev-parse", "--verify", "refs/remotes/origin/main"]).catch(() => null);
const remote = parseRemoteHead(await git(["ls-remote", "origin", "refs/heads/main"]));
const report = {
  head,
  branch: branch || null,
  cachedOriginMain: tracking,
  remoteOriginMain: remote,
  cachedRemoteStale: Boolean(tracking && remote && tracking !== remote),
  headIsRemoteMain: Boolean(remote && head === remote),
};

if (jsonMode) {
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
} else {
  console.log(`HEAD:               ${report.head}`);
  console.log(`branch:             ${report.branch ?? "(detached)"}`);
  console.log(`cached origin/main: ${report.cachedOriginMain ?? "unavailable"}`);
  console.log(`actual origin/main: ${report.remoteOriginMain ?? "unavailable"}`);
  console.log(`cached remote stale:${report.cachedRemoteStale ? " YES" : " no"}`);
  console.log(`HEAD is remote main:${report.headIsRemoteMain ? " yes" : " no"}`);
}

if (requireCurrent && !report.headIsRemoteMain) process.exitCode = 2;
