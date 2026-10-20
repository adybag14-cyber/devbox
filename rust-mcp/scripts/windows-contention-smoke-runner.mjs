import assert from "node:assert/strict";
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

if (process.platform !== "win32") {
  console.log("Windows contention smoke skipped on non-Windows runner.");
  process.exit(0);
}

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDir, "..", "..");
const binaryPath = process.env.DEVBOX_MCP_TEST_BINARY || path.join(projectRoot, "rust-mcp", "target", "debug", "devbox-mcp.exe");

const reservePort = () => new Promise((resolve, reject) => {
  const server = net.createServer();
  server.once("error", reject);
  server.listen(0, "127.0.0.1", () => {
    const address = server.address();
    assert.ok(address && typeof address === "object");
    const port = address.port;
    server.close((error) => error ? reject(error) : resolve(port));
  });
});

const waitForHealth = async (url) => {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(new URL("readyz", url), { signal: AbortSignal.timeout(1_000) });
      if (response.ok && (await response.json()).ok === true) return;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("contention smoke server did not become healthy");
};

const port = await reservePort();
const baseUrl = new URL(`http://127.0.0.1:${port}/`);
const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-contention-"));
const shimDir = path.join(runtimeDir, "bin");
await mkdir(shimDir, { recursive: true });
await writeFile(path.join(shimDir, "rg.cmd"), "@echo off\r\necho ripgrep-ci-shim 1.0\r\n", "utf8");
const server = spawn(binaryPath, [], {
  cwd: projectRoot,
  env: {
    ...process.env,
    PATH: `${shimDir};${process.env.PATH ?? ""}`,
    DEVBOX_PROJECT_ROOT: runtimeDir,
    HOST_WORKSPACE_PATH: projectRoot,
    HOST: "127.0.0.1",
    PORT: String(port),
    MCP_AUTH_MODE: "none",
    PUBLIC_BASE_URL: "",
    DEVBOX_RUNTIME_MODE: "host",
    ENABLE_HOST_EXEC: "true",
    HOST_DEFAULT_WORKDIR: projectRoot,
    MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
    MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "slots"),
    MCP_EXEC_MAX_CONCURRENT: "6",
    MCP_EXEC_RESERVED_INTERACTIVE: "1",
    MCP_EXEC_HEAVY_CAPACITY: "4",
    MCP_EXEC_HEAVY_WEIGHT: "2",
    MCP_WATCH_MAX_CONCURRENT: "4",
    MCP_JOB_HEARTBEAT_MS: "1000",
    MCP_JOB_ORPHAN_STALE_MS: "3000",
    HOST_PROGRAM_ALLOWLIST: "powershell,pwsh,cmd,git,gh,docker,node,npm,npx,python,py,pip,rg,curl,winget",
    DEVBOX_PROGRAM_ALLOWLIST: "powershell,pwsh,cmd,git,gh,docker,node,npm,npx,python,py,pip,rg,curl,winget",
  },
  stdio: ["ignore", "pipe", "pipe"],
  windowsHide: true,
});
let stderr = "";
server.stderr.setEncoding("utf8");
server.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-16_000); });
server.stdout.resume();

const transport = new StreamableHTTPClientTransport(baseUrl);
const client = new Client({ name: "windows-contention-smoke", version: "0.1.0" });
const verifyFailureCleanup = process.argv.includes("--verify-failure-cleanup");
const controlledFailure = new Error("Controlled assertion failure after watcher readiness");
let exercisedFailureCleanup = false;
const jobs = [];
const readyFiles = [];
const releaseFile = path.join(runtimeDir, "release-watchers");
let stopProbes = false, probeTask;
const diagnosticRoot = path.join(projectRoot, ".cpp-build");
await mkdir(diagnosticRoot, { recursive: true });
await writeFile(path.join(diagnosticRoot, "windows-contention-owner.json"), JSON.stringify({
  pid: process.pid, serverPid: server.pid, binaryPath, runtimeDir, startedAt: new Date().toISOString(),
}, null, 2));
try {
  await waitForHealth(baseUrl);
  await client.connect(transport);
  for (let index = 0; index < 4; index += 1) {
    const readyFile = path.join(runtimeDir, `watcher-${index}-ready`);
    const result = await client.callTool({
      name: "devbox_run_program_start",
      arguments: {
        program: "node",
        args: ["-e", "const fs=require('node:fs');fs.writeFileSync(process.argv[1],'ready');setInterval(()=>{if(fs.existsSync(process.argv[2]))process.exit(0)},100)", readyFile, releaseFile],
        working_dir: projectRoot,
        timeout_seconds: 60,
        resource_class: "watch",
      },
    });
    assert.equal(result.isError, false);
    const id = result.structuredContent?.data?.id;
    assert.equal(typeof id, "string", JSON.stringify(result.structuredContent));
    jobs.push(id);
    readyFiles.push(readyFile);
  }
  assert.equal(jobs.filter(Boolean).length, 4);
  // Measure watcher deadlines only after the actual child workloads are alive.
  // A fixed sleep measured from submission conflates Windows launch latency
  // with the eight-second wait contract and can expire during later setup.
  const readyBy = Date.now() + 20000;
  for (let index = 0; index < jobs.length; index += 1) {
    for (;;) {
      try { if (await readFile(readyFiles[index], "utf8") === "ready") break; }
      catch (error) { if (error.code !== "ENOENT") throw error; }
      const status = await client.callTool({name:"devbox_job_status",arguments:{job_id:jobs[index]}});
      const data = status.structuredContent?.data;
      assert.equal(status.isError,false,JSON.stringify(status.structuredContent));
      assert(!["succeeded","failed","cancelled","timed_out","interrupted"].includes(data?.status),
        `Watcher child ended before readiness: ${JSON.stringify(data)}`);
      assert(Date.now() < readyBy,`Watcher child did not reach readiness: ${JSON.stringify(data)}`);
      await new Promise(resolve=>setTimeout(resolve,100));
    }
  }
  if (verifyFailureCleanup) {
    exercisedFailureCleanup = true;
    throw controlledFailure;
  }

  const probeSamples = [];
  probeTask = (async () => {
    while (!stopProbes) {
      const started = performance.now();
      try {
        const response = await fetch(new URL("readyz", baseUrl), { signal: AbortSignal.timeout(1_500) });
        assert.equal(response.status, 200);
        assert.equal((await response.json()).ok, true);
        probeSamples.push(performance.now() - started);
      } catch (error) {
        probeSamples.push(Number.POSITIVE_INFINITY);
        throw error;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  })();
  probeTask.catch(() => {});

  const waits = jobs.map(async (jobId) => {
    const started = performance.now();
    const result = await client.callTool({
      name: "devbox_job_status",
      arguments: { job_id: jobId, wait_seconds: 8, terminal_only: true },
    });
    const elapsed = performance.now() - started;
    assert.equal(result.isError, false);
    assert.equal(result.structuredContent?.data?.waitTimedOut, true,
      `Expected an active job to exhaust the wait deadline: ${JSON.stringify(result.structuredContent)}`);
    assert.ok(elapsed >= 7_500, `wait returned too early: ${elapsed}ms`);
    assert.ok(elapsed <= 9_500, `8s wait exceeded hard deadline budget: ${elapsed}ms`);
    return elapsed;
  });
  const waitDurations = await Promise.all(waits);
  stopProbes = true;
  await probeTask;

  const statusStarted = performance.now();
  const status = await client.callTool({ name: "devbox_status", arguments: {} });
  const statusMs = performance.now() - statusStarted;
  assert.equal(status.isError, false);
  assert.ok(statusMs < 1_500, `devbox_status was unexpectedly slow: ${statusMs}ms`);
  assert.equal(status.structuredContent?.data?.processProbe?.backend, "win32-openprocess");
  assert.equal(status.structuredContent?.data?.executionStore?.ok, true);
  assert.equal(status.structuredContent?.data?.executionStore?.jobsWritable, true);
  assert.equal(status.structuredContent?.data?.executionStore?.schedulerWritable, true);
  assert.ok(status.structuredContent?.data?.executionStore?.freeBytes > 0);

  const falsePositive = client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "node",
      args: ["-e", "/* cargo test */ setTimeout(()=>{},2200)"],
      working_dir: projectRoot,
      timeout_seconds: 10,
      max_output_chars: 1_024,
    },
  });
  let falsePositiveStatus = null;
  const falsePositiveDeadline = Date.now() + 4_000;
  while (Date.now() < falsePositiveDeadline) {
    const candidate = await client.callTool({ name: "devbox_status", arguments: {} });
    const matching = candidate.structuredContent?.data?.execution?.occupied_slots?.find((slot) => slot.label?.startsWith("devbox_run_program:node"));
    if (matching) {
      falsePositiveStatus = matching;
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  assert.ok(falsePositiveStatus, "structured Node classifier fixture never became visible");
  assert.equal(falsePositiveStatus.resourceClass, "light", "payload text incorrectly classified Node as heavy");
  assert.equal((await falsePositive).isError, false);

  const heavyFixture = path.join(runtimeDir, "heavy-fixture");
  await mkdir(heavyFixture, { recursive: true });
  await writeFile(path.join(heavyFixture, "package.json"), JSON.stringify({
    private: true,
    scripts: { test: "node -e \"setTimeout(()=>{},1200)\"" },
  }), "utf8");
  const heavyCalls = [0, 1].map(() => client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "npm",
      args: ["test", "--silent"],
      working_dir: heavyFixture,
      timeout_seconds: 10,
      max_output_chars: 1_024,
    },
  }));
  const pressureConstrained = ["warning", "critical"].includes(
    status.structuredContent?.data?.executionStore?.diskPressure,
  );
  const expectedHeavyOccupied = pressureConstrained ? 2 : 4;
  let weightedStatus = null;
  const weightedDeadline = Date.now() + 2_000;
  while (Date.now() < weightedDeadline) {
    const candidate = await client.callTool({ name: "devbox_status", arguments: {} });
    const execution = candidate.structuredContent?.data?.execution;
    if (execution?.occupied === expectedHeavyOccupied) {
      weightedStatus = execution;
      break;
    }
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  assert.ok(
    weightedStatus,
    pressureConstrained
      ? "disk-pressure warning did not serialize synchronous heavy commands"
      : "two synchronous heavy commands did not consume weighted capacity",
  );
  assert.equal(weightedStatus.heavy_capacity, 4);
  assert.equal(weightedStatus.occupied_slots.length, expectedHeavyOccupied);
  assert.ok(weightedStatus.occupied_slots.every((slot) => slot.resourceClass === "heavy" && slot.weight === 2));
  const lightDuringHeavyStarted = performance.now();
  const lightDuringHeavy = await client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "node",
      args: ["-e", "process.stdout.write('light-capacity-ok')"],
      working_dir: projectRoot,
      timeout_seconds: 10,
      max_output_chars: 1_024,
    },
  });
  assert.equal(lightDuringHeavy.isError, false);
  assert.ok(performance.now() - lightDuringHeavyStarted < 1_000, "heavy workload starved light interactive capacity");
  for (const result of await Promise.all(heavyCalls)) assert.equal(result.isError, false);

  const pressure = await client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "node",
      args: ["-e", "const chunk='x'.repeat(1024*1024); for(let i=0;i<128;i++) process.stdout.write(chunk);"],
      working_dir: projectRoot,
      timeout_seconds: 30,
      max_output_chars: 4_096,
    },
  });
  assert.equal(pressure.isError, false);
  const pressureOutput = pressure.structuredContent?.data?.output || {};
  assert.equal(pressureOutput.stdout_capture_truncated, true);
  assert.ok(pressureOutput.stdout_original_chars >= 128 * 1024 * 1024);
  const pressureStatus = await client.callTool({ name: "devbox_status", arguments: {} });
  const peakRequested = pressureStatus.structuredContent?.data?.performance?.process?.memory?.allocator?.peakRequestedBytes;
  assert.ok(Number.isFinite(peakRequested) && peakRequested < 64 * 1024 * 1024, `foreground capture allocator peak was ${peakRequested}`);

  for (const program of ["npm", "npx", "rg"]) {
    const result = await client.callTool({
      name: "devbox_run_program",
      arguments: { program, args: ["--version"], working_dir: projectRoot, timeout_seconds: 15, max_output_chars: 4_096 },
    });
    assert.equal(result.isError, false, `${program} direct runner failed: ${JSON.stringify(result.structuredContent)}`);
  }

  for (const jobId of jobs) {
    const result = await client.callTool({ name: "devbox_job_cancel", arguments: { job_id: jobId } });
    assert.equal(result.isError, false);
  }
  assert.ok(probeSamples.length >= 20, `expected repeated health probes, got ${probeSamples.length}`);
  const maxProbe = Math.max(...probeSamples);
  assert.ok(Number.isFinite(maxProbe) && maxProbe < 1_500, `health probe stalled under watchers: ${maxProbe}ms`);
  console.log(JSON.stringify({ waitDurationsMs: waitDurations, healthProbeCount: probeSamples.length, maxHealthMs: maxProbe, statusMs }));
 } catch (error) {
  const states = [];
  for (const id of jobs) {
    assert.match(id, /^[a-zA-Z0-9_-]{1,128}$/u);
    const record = {id};
    for (const file of ["status.json", "heartbeat.json", "stdout.log", "stderr.log"]) {
      try {
        const value = await readFile(path.join(runtimeDir, "jobs", id, file), "utf8");
        record[file] = value.slice(-16000);
      } catch (readError) { record[file] = readError.code || String(readError); }
    }
    states.push(record);
  }
  await writeFile(path.join(diagnosticRoot, "windows-contention-failure.json"), JSON.stringify({
    kind: error===controlledFailure ? "controlled_cleanup_test" : "unexpected_failure",
    error: String(error), states, serverStderr: stderr,
  }, null, 2));
  if (error !== controlledFailure) throw error;
 } finally {
  stopProbes = true;
  await probeTask?.catch(() => {});
  await writeFile(releaseFile, "stop");
  // Detached job runners need cancellation even when a preceding assertion
  // fails. Preserve their state if shutdown cannot be confirmed.
  let jobsStopped = true;
  const recorded = await readdir(path.join(runtimeDir, "jobs"), {withFileTypes:true}).catch(error=>{
    if(error.code==='ENOENT')return [];throw error;
  });
  const cleanupJobs = [...new Set([...jobs,...recorded.filter(row=>row.isDirectory()&&/^[a-zA-Z0-9_-]{1,128}$/u.test(row.name)).map(row=>row.name)])];
  if(cleanupJobs.length>16)jobsStopped=false;
  for (const id of cleanupJobs.slice(0,16)) {
    try {
      const cancelled = await client.callTool({name:"devbox_job_cancel",arguments:{job_id:id}});
      assert.equal(cancelled.isError,false,JSON.stringify(cancelled.structuredContent));
      const until = Date.now() + 10000;
      let stopped = false;
      do {
        const state = await client.callTool({name:"devbox_job_status",arguments:{job_id:id,wait_seconds:1,terminal_only:true}});
        const data = state.structuredContent?.data;
        stopped = !state.isError && data?.runnerAlive !== true &&
          ["succeeded","failed","cancelled","timed_out","interrupted"].includes(data?.status);
        if (stopped) break;
        await new Promise(resolve=>setTimeout(resolve,100));
      } while (Date.now() < until);
      if (!stopped) jobsStopped = false;
    } catch (error) {
      jobsStopped = false;
      process.stderr.write(`Owned contention job cleanup failed for ${id}: ${String(error)}\n`);
    }
  }
  await client.close().catch(() => {});
  // The server is process-scoped to this smoke; killing it below owns transport cleanup.
  if (server.exitCode === null && server.signalCode === null) {
    const exited = new Promise((resolve) => server.once("exit", resolve));
    server.kill();
    await Promise.race([exited, new Promise((resolve) => setTimeout(resolve, 5_000))]);
  }
  if (jobsStopped && (server.exitCode !== null || server.signalCode !== null))
    await rm(runtimeDir, { recursive: true, force: true });
  else {
    process.stderr.write(`Retained owned contention fixture for cleanup: ${runtimeDir}\n`);
    throw new Error("Owned contention jobs or server did not stop; fixture retained");
  }
  if (stderr) process.stderr.write(stderr);
}
if (verifyFailureCleanup) {
  assert(exercisedFailureCleanup);
  console.log(JSON.stringify({ok:true,check:"assertion failure cancels owned runners and removes only a stopped fixture"}));
}
