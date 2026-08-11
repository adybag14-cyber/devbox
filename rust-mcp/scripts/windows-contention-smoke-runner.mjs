import assert from "node:assert/strict";
import { mkdtemp, rm } from "node:fs/promises";
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
const binaryPath = path.join(projectRoot, "rust-mcp", "target", "debug", "devbox-mcp.exe");

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
      const response = await fetch(new URL("healthz", url), { signal: AbortSignal.timeout(1_000) });
      if (response.ok && await response.text() === "ok") return;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("contention smoke server did not become healthy");
};

const port = await reservePort();
const baseUrl = new URL(`http://127.0.0.1:${port}/`);
const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-contention-"));
const server = spawn(binaryPath, [], {
  cwd: projectRoot,
  env: {
    ...process.env,
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
    MCP_EXEC_MAX_CONCURRENT: "4",
    MCP_EXEC_RESERVED_INTERACTIVE: "1",
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

const transport = new StreamableHTTPClientTransport(baseUrl);
const client = new Client({ name: "windows-contention-smoke", version: "0.1.0" });
try {
  await waitForHealth(baseUrl);
  await client.connect(transport);
  const jobs = [];
  for (let index = 0; index < 4; index += 1) {
    const result = await client.callTool({
      name: "devbox_run_program_start",
      arguments: {
        program: "node",
        args: ["-e", "setTimeout(()=>{},20000)"],
        working_dir: projectRoot,
        timeout_seconds: 30,
        resource_class: "watch",
      },
    });
    assert.equal(result.isError, false);
    jobs.push(result.structuredContent?.data?.id);
  }
  assert.equal(jobs.filter(Boolean).length, 4);

  const probeSamples = [];
  let stopProbes = false;
  const probeTask = (async () => {
    while (!stopProbes) {
      const started = performance.now();
      try {
        const response = await fetch(new URL("healthz", baseUrl), { signal: AbortSignal.timeout(1_500) });
        assert.equal(response.status, 200);
        assert.equal(await response.text(), "ok");
        probeSamples.push(performance.now() - started);
      } catch (error) {
        probeSamples.push(Number.POSITIVE_INFINITY);
        throw error;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  })();

  const waits = jobs.map(async (jobId) => {
    const started = performance.now();
    const result = await client.callTool({
      name: "devbox_job_status",
      arguments: { job_id: jobId, wait_seconds: 8, terminal_only: true },
    });
    const elapsed = performance.now() - started;
    assert.equal(result.isError, false);
    assert.equal(result.structuredContent?.data?.waitTimedOut, true);
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

  for (const program of ["npm", "rg"]) {
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
 } finally {
  await client.close().catch(() => {});
  if (server.exitCode === null && server.signalCode === null) {
    const exited = new Promise((resolve) => server.once("exit", resolve));
    server.kill();
    await Promise.race([exited, new Promise((resolve) => setTimeout(resolve, 5_000))]);
  }
  await rm(runtimeDir, { recursive: true, force: true }).catch(() => {});
  if (stderr) process.stderr.write(stderr);
}
