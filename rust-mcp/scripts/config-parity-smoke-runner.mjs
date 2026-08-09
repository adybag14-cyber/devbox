import assert from "node:assert/strict";
import { access, mkdtemp, rm, writeFile } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDir, "..", "..");
const binaryPath = path.join(projectRoot, "rust-mcp", "target", "debug", process.platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const rmWithRetry = async (target) => {
  let lastError;
  for (let attempt = 0; attempt < 20; attempt += 1) {
    try {
      await rm(target, { recursive: true, force: true, maxRetries: 2, retryDelay: 100 });
      return;
    } catch (error) {
      lastError = error;
      await sleep(100);
    }
  }
  throw lastError;
};

const reservePort = () => new Promise((resolve, reject) => {
  const server = net.createServer();
  server.unref();
  server.once("error", reject);
  server.listen(0, "127.0.0.1", () => {
    const address = server.address();
    assert.ok(address && typeof address === "object");
    server.close((error) => error ? reject(error) : resolve(address.port));
  });
});

const startServer = async (overrides = {}) => {
  const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-config-"));
  const workspace = path.join(runtimeDir, "workspace");
  await writeFile(path.join(runtimeDir, ".keep"), "");
  await import("node:fs/promises").then(({ mkdir }) => mkdir(workspace, { recursive: true }));
  const port = await reservePort();
  const env = {
    ...process.env,
    DEVBOX_PROJECT_ROOT: runtimeDir,
    HOST_WORKSPACE_PATH: workspace,
    HOST_DEFAULT_WORKDIR: workspace,
    HOST: "127.0.0.1",
    PORT: String(port),
    MCP_AUTH_MODE: "none",
    PUBLIC_BASE_URL: "",
    DEVBOX_RUNTIME_MODE: "host",
    MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
    MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "slots"),
    ...overrides,
  };
  delete env.ENABLE_HOST_EXEC;
  const child = spawn(binaryPath, [], { cwd: projectRoot, env, stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  const exited = new Promise((resolve, reject) => {
    child.once("error", reject);
    child.once("exit", (code, signal) => resolve({ code, signal }));
  });
  const baseUrl = new URL(`http://127.0.0.1:${port}/`);
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const early = await Promise.race([exited.then((result) => ({ result })), sleep(100).then(() => null)]);
    if (early) throw new Error(`server exited early ${JSON.stringify(early.result)}\n${stdout}\n${stderr}`);
    try {
      const response = await fetch(new URL("healthz", baseUrl));
      if (response.ok) return { runtimeDir, workspace, child, exited, baseUrl, logs: () => ({ stdout, stderr }) };
    } catch {}
  }
  throw new Error(`server failed readiness\n${stdout}\n${stderr}`);
};

const stopServer = async (server) => {
  if (server.child.exitCode === null && server.child.signalCode === null) {
    server.child.kill();
    await Promise.race([server.exited, sleep(5_000)]);
  }
  await sleep(150);
  await rmWithRetry(server.runtimeDir);
};

await access(binaryPath);
const configured = await startServer({
  ENABLE_WINDOWS_HOST_EXEC: "true",
  MAX_TEXT_OUTPUT_CHARS: "20000",
  MAX_COMMAND_OUTPUT_CHARS: "65536",
  HOST_SEARCH_BACKEND: "js",
});
try {
  await writeFile(path.join(configured.workspace, "search.txt"), "alpha parity needle omega\n");
  const client = new Client({ name: "config-parity-smoke", version: "1.0.0" });
  await client.connect(new StreamableHTTPClientTransport(configured.baseUrl));
  const listed = await client.listTools();
  for (const name of ["devbox_exec", "devbox_exec_readonly", "devbox_run_program", "host_exec", "windows_host_exec", "host_run_program", "windows_host_run_program"]) {
    const tool = listed.tools.find((candidate) => candidate.name === name);
    assert.ok(tool, `missing ${name}`);
    const outputSchema = tool.inputSchema?.properties?.max_output_chars;
    assert.equal(outputSchema?.minimum, 100, `${name} minimum`);
    assert.equal(outputSchema?.maximum, 20_000, `${name} maximum`);
    assert.equal(outputSchema?.default, 20_000, `${name} default`);
    assert.equal(outputSchema?.type, "integer", `${name} type`);
  }
  const bounded = await client.callTool({
    name: "devbox_run_program",
    arguments: { program: "node", args: ["-e", "process.stdout.write('x'.repeat(25000))"] },
  });
  assert.equal(bounded.isError, false);
  assert.equal(bounded.structuredContent?.truncated, true);
  assert.ok((bounded.structuredContent?.stdout || "").length <= 20_000);
  assert.equal(bounded.structuredContent?.data?.output?.max_chars, 20_000);

  const rejected = await client.callTool({
    name: "devbox_run_program",
    arguments: { program: "node", args: ["--version"], max_output_chars: 20_001 },
  });
  assert.equal(rejected.isError, true);

  const search = await client.callTool({
    name: "devbox_search_files",
    arguments: { pattern: "parity needle", path: configured.workspace, max_matches: 10 },
  });
  assert.equal(search.isError, false);
  assert.match(search.structuredContent?.stdout || "", /parity needle/);
  assert.doesNotMatch(search.structuredContent?.stderr || "", /ripgrep/i);
  await client.close();
} finally {
  await stopServer(configured);
}

const legacyDisabled = await startServer({ ENABLE_WINDOWS_HOST_EXEC: "false" });
try {
  const metadata = await (await fetch(legacyDisabled.baseUrl)).json();
  assert.equal(metadata.runtime?.hostExecEnabled, false);
} finally {
  await stopServer(legacyDisabled);
}

const invalidDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-invalid-search-"));
try {
  const port = await reservePort();
  const env = {
    ...process.env,
    DEVBOX_PROJECT_ROOT: invalidDir,
    HOST_WORKSPACE_PATH: invalidDir,
    HOST_DEFAULT_WORKDIR: invalidDir,
    HOST: "127.0.0.1",
    PORT: String(port),
    MCP_AUTH_MODE: "none",
    PUBLIC_BASE_URL: "",
    DEVBOX_RUNTIME_MODE: "host",
    HOST_SEARCH_BACKEND: "not-a-backend",
  };
  const child = spawn(binaryPath, [], { cwd: projectRoot, env, stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
  let stderr = "";
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  const result = await new Promise((resolve, reject) => {
    child.once("error", reject);
    child.once("exit", (code, signal) => resolve({ code, signal }));
  });
  assert.notEqual(result.code, 0);
  assert.match(stderr, /Unsupported HOST_SEARCH_BACKEND/);
} finally {
  await rmWithRetry(invalidDir);
}

console.log(JSON.stringify({ ok: true, commandOutputLimit: 20_000, forcedSearchBackend: "js", legacyHostExecAlias: true, invalidBackendRejected: true }, null, 2));
