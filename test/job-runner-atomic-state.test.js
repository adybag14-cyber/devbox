import test from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { access, mkdir, mkdtemp, readFile, readdir, realpath, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { writeJsonAtomic } from "../src/atomic-json.js";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const exists = file => access(file).then(() => true, () => false);
async function waitFor(check, label, timeout = 10000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) {
    const value = await check();
    if (value) return value;
    await sleep(10);
  }
  throw new Error(`Timed out waiting for ${label}`);
}
async function stopOwned(child) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const stopped = once(child, "exit");
  child.kill();
  await stopped;
}

for (const permanent of [false, true]) test(permanent ? "detached job explicitly reports a permanent terminal write failure" : "detached job preserves terminal success across a temporary Windows reader lock", {skip: process.platform !== "win32", timeout: 30000}, async () => {
  const parent = await realpath(os.tmpdir());
  const root = await mkdtemp(path.join(parent, "devbox-job-atomic-"));
  const job = path.join(root, "job");
  await mkdir(job);
  const statusPath = path.join(job, "status.json"), requestPath = path.join(job, "request.json");
  const ready = path.join(root, "lock-ready"), release = path.join(root, "lock-release"), finish = path.join(root, "child-finish");
  const childScript = "const fs=require('node:fs');const done=process.argv[1];const timer=setInterval(()=>{if(fs.existsSync(done)){clearInterval(timer);console.log('ATOMIC_JOB_DONE')}},10);setTimeout(()=>{clearInterval(timer);process.exitCode=9},15000).unref();";
  const id = "job-atomic-fixture";
  await writeFile(statusPath, JSON.stringify({id, status: "queued"}));
  await writeFile(requestPath, JSON.stringify({id, mode: "program", program: "node", args: ["-e", childScript, finish], workingDir: root, timeoutMs: 20000, runtimeMode: "host", resourceClass: "light", createdAtUtc: new Date().toISOString()}));
  let runner, lock, runnerError = "", lockError = "";
  try {
    runner = spawn(process.execPath, [path.join(repo, "scripts/devbox-job-runner.mjs"), requestPath], {
      cwd: repo, windowsHide: true, stdio: ["ignore", "ignore", "pipe"],
      env: {...process.env, DEVBOX_RUNTIME_MODE: "host", HOST_WORKSPACE_PATH: root, HOST_DEFAULT_WORKDIR: root,
        DEVBOX_WORKSPACE_PATH: root, DEVBOX_PROGRAM_ALLOWLIST: "node", HOST_PROGRAM_ALLOWLIST: "node",
        MCP_EXEC_SLOT_ROOT: path.join(root, "slots"), MCP_JOBS_ROOT: path.join(root, "jobs"),
        MCP_EXEC_MAX_CONCURRENT: "2", MCP_EXEC_RESERVED_INTERACTIVE: "0"},
    });
    runner.stderr.on("data", bytes => {runnerError = (runnerError + bytes).slice(-5000);});
    const exited = once(runner, "exit");
    await waitFor(async () => {
      const value = await readFile(statusPath, "utf8").then(JSON.parse);
      assert.equal(runner.exitCode, null, runnerError);
      return value.status === "running";
    }, "runner to execute");
    lock = spawn("powershell.exe", ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
      path.join(repo, "test/fixtures/hold-job-status.ps1"), "-StatusPath", statusPath, "-ReadyPath", ready, "-ReleasePath", release],
      {windowsHide: true, stdio: ["ignore", "ignore", "pipe"]});
    lock.stderr.on("data", bytes => {lockError = (lockError + bytes).slice(-2000);});
    await waitFor(async () => {
      assert.equal(lock.exitCode, null, lockError);
      return exists(ready);
    }, "native reader lock");
    await writeFile(finish, "finish");
    await waitFor(async () => {
      const files = await readdir(job);
      for (const file of files.filter(name => name.startsWith("status.json.") && name.endsWith(".tmp"))) {
        const value = await readFile(path.join(job, file), "utf8").then(JSON.parse).catch(() => null);
        if (value?.status === "succeeded") return true;
      }
      assert.equal(runner.exitCode, null, runnerError);
      return false;
    }, "staged terminal record");
    // Keep the actual Windows sharing violation in place across the first rename.
    await sleep(60);
    if (!permanent) await writeFile(release, "release");
    const [code] = await exited;
    assert.equal(code, permanent ? 1 : 0, `Unexpected runner outcome: ${runnerError}`);
    const final = JSON.parse(await readFile(statusPath, "utf8"));
    assert.equal(final.status, permanent ? "running" : "succeeded");
    if (permanent) {
      const diagnostic = JSON.parse(runnerError.trim());
      assert.equal(diagnostic.error, "JOB_STATUS_PERSIST_FAILED");
      assert.equal(diagnostic.phase, "terminal");
      assert(diagnostic.message.length <= 800);
      assert.equal(final.completedAtUtc, null);
    } else assert.equal(final.exitCode, 0);
    assert.match(await readFile(path.join(job, "stdout.log"), "utf8"), /ATOMIC_JOB_DONE/);
    assert.deepEqual((await readdir(job)).filter(name => name.startsWith("status.json.") && name.endsWith(".tmp")), []);
  } finally {
    await writeFile(finish, "finish").catch(() => {});
    await writeFile(release, "release").catch(() => {});
    if (runner?.exitCode === null) await Promise.race([once(runner, "exit"), sleep(2000)]);
    if (lock?.exitCode === null) await Promise.race([once(lock, "exit"), sleep(2000)]);
    await stopOwned(runner); await stopOwned(lock);
    assert.equal(path.dirname(await realpath(root)), parent);
    assert.equal(await realpath(root), root);
    await rm(root, {recursive: true, force: true});
  }
});

test("failed queued-state persistence reports failure before running a command", {timeout: 15000}, async () => {
  const parent = await realpath(os.tmpdir());
  const root = await mkdtemp(path.join(parent, "devbox-queued-state-"));
  const job = path.join(root, "job"), marker = path.join(root, "must-not-run");
  await mkdir(path.join(job, "status.json"), {recursive: true});
  const requestPath = path.join(job, "request.json");
  await writeFile(requestPath, JSON.stringify({id: "job-queued-fixture", mode: "program", program: "node",
    args: ["-e", "require('node:fs').writeFileSync(process.argv[1],'ran')", marker], workingDir: root,
    timeoutMs: 5000, runtimeMode: "host", resourceClass: "light", createdAtUtc: new Date().toISOString()}));
  let runner, errorText = "";
  try {
    runner = spawn(process.execPath, [path.join(repo, "scripts/devbox-job-runner.mjs"), requestPath], {
      cwd: repo, windowsHide: true, stdio: ["ignore", "ignore", "pipe"],
      env: {...process.env, DEVBOX_RUNTIME_MODE: "host", MCP_EXEC_SLOT_ROOT: path.join(root, "slots"),
        MCP_JOBS_ROOT: path.join(root, "jobs"), HOST_WORKSPACE_PATH: root, HOST_DEFAULT_WORKDIR: root,
        DEVBOX_WORKSPACE_PATH: root, DEVBOX_PROGRAM_ALLOWLIST: "node", HOST_PROGRAM_ALLOWLIST: "node"},
    });
    runner.stderr.on("data", bytes => {errorText = (errorText + bytes).slice(-5000);});
    const [code] = await once(runner, "exit");
    assert.equal(code, 1);
    const diagnostic = JSON.parse(errorText.trim());
    assert.equal(diagnostic.error, "JOB_STATUS_PERSIST_FAILED");
    assert.equal(diagnostic.phase, "queued");
    assert(diagnostic.message.length <= 800);
    assert.equal(await exists(marker), false);
    assert.equal(await exists(path.join(job, "stdout.log")), false);
  } finally {
    await stopOwned(runner);
    assert.equal(path.dirname(await realpath(root)), parent);
    assert.equal(await realpath(root), root);
    await rm(root, {recursive: true, force: true});
  }
});

test("persistent Windows replacement denial preserves the previous complete record", {skip: process.platform !== "win32", timeout: 15000}, async () => {
  const parent = await realpath(os.tmpdir());
  const root = await mkdtemp(path.join(parent, "devbox-atomic-denied-"));
  const statusPath = path.join(root, "state.json"), ready = path.join(root, "ready"), release = path.join(root, "release");
  let lock;
  try {
    await writeJsonAtomic(statusPath, {state: "known-good"});
    const original = await readFile(statusPath, "utf8");
    lock = spawn("powershell.exe", ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
      path.join(repo, "test/fixtures/hold-job-status.ps1"), "-StatusPath", statusPath, "-ReadyPath", ready, "-ReleasePath", release],
      {windowsHide: true, stdio: "ignore"});
    const stopped = once(lock, "exit");
    await waitFor(() => exists(ready), "persistent native lock");
    await assert.rejects(writeJsonAtomic(statusPath, {state: "replacement"}), error => ["EACCES", "EPERM", "EBUSY", "EEXIST"].includes(error?.code));
    assert.equal(await readFile(statusPath, "utf8"), original);
    assert.deepEqual((await readdir(root)).filter(name => name.endsWith(".tmp")), []);
    await writeFile(release, "release");
    const [code] = await stopped;
    assert.equal(code, 0);
    await writeJsonAtomic(statusPath, {state: "recovered"});
    assert.deepEqual(JSON.parse(await readFile(statusPath, "utf8")), {state: "recovered"});
  } finally {
    await writeFile(release, "release").catch(() => {});
    if (lock?.exitCode === null) await Promise.race([once(lock, "exit"), sleep(2000)]);
    await stopOwned(lock);
    assert.equal(path.dirname(await realpath(root)), parent);
    assert.equal(await realpath(root), root);
    await rm(root, {recursive: true, force: true});
  }
});
