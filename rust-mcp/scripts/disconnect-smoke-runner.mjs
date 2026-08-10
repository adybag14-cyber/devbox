import assert from "node:assert/strict";
import { access, mkdtemp, readFile, rm } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDir, "..", "..");
const binaryPath = path.join(projectRoot, "rust-mcp", "target", "debug", process.platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp");

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

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const processAlive = (pid) => {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
};

await access(binaryPath);
const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-disconnect-"));
const pidFile = path.join(runtimeDir, "disconnect-pids.json");
const port = await reservePort();
let stdout = "";
let stderr = "";
const server = spawn(binaryPath, [], {
  cwd: projectRoot,
  env: {
    ...process.env,
    DEVBOX_PROJECT_ROOT: runtimeDir,
    HOST_WORKSPACE_PATH: projectRoot,
    HOST_DEFAULT_WORKDIR: projectRoot,
    HOST: "127.0.0.1",
    PORT: String(port),
    MCP_AUTH_MODE: "none",
    PUBLIC_BASE_URL: "",
    DEVBOX_RUNTIME_MODE: "host",
    ENABLE_HOST_EXEC: "true",
    MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
    MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "execution-slots"),
  },
  stdio: ["ignore", "pipe", "pipe"],
  windowsHide: true,
});
server.stdout.setEncoding("utf8");
server.stderr.setEncoding("utf8");
server.stdout.on("data", (chunk) => { stdout = `${stdout}${chunk}`.slice(-16_000); });
server.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-16_000); });
const exited = new Promise((resolve, reject) => {
  server.once("error", reject);
  server.once("exit", (code, signal) => resolve({ code, signal }));
});
let socket;

try {
  const deadline = Date.now() + 30_000;
  let ready = false;
  while (Date.now() < deadline) {
    const early = await Promise.race([exited.then((result) => ({ result })), sleep(100).then(() => null)]);
    if (early) throw new Error(`disconnect smoke server exited early ${JSON.stringify(early.result)}\n${stdout}\n${stderr}`);
    try {
      const response = await fetch(`http://127.0.0.1:${port}/healthz`);
      if (response.ok && (await response.text()) === "ok") {
        ready = true;
        break;
      }
    } catch {}
  }
  if (!ready) throw new Error(`disconnect smoke server failed readiness\n${stdout}\n${stderr}`);

  const childScript = [
    "const fs=require('fs');",
    "const {spawn}=require('child_process');",
    "const child=spawn(process.execPath,['-e','setTimeout(()=>{},30000)'],{stdio:'ignore'});",
    `fs.writeFileSync(${JSON.stringify(pidFile)},JSON.stringify({parent:process.pid,child:child.pid}));`,
    "setTimeout(()=>{},30000);",
  ].join("");
  const body = JSON.stringify({
    jsonrpc: "2.0",
    id: 88001,
    method: "tools/call",
    params: {
      name: "devbox_run_program",
      arguments: {
        program: "node",
        args: ["-e", childScript],
        timeout_seconds: 40,
        max_output_chars: 2000,
      },
    },
  });

  let responseBytes = "";
  socket = net.createConnection({ host: "127.0.0.1", port });
  socket.setEncoding("utf8");
  socket.on("data", (chunk) => { responseBytes += chunk; });
  await new Promise((resolve, reject) => {
    socket.once("connect", resolve);
    socket.once("error", reject);
  });
  socket.write([
    "POST / HTTP/1.1",
    `Host: 127.0.0.1:${port}`,
    "Content-Type: application/json",
    "Accept: application/json, text/event-stream",
    "MCP-Protocol-Version: 2025-06-18",
    "User-Agent: rust-disconnect-smoke",
    `Content-Length: ${Buffer.byteLength(body)}`,
    "Connection: keep-alive",
    "",
    body,
  ].join("\r\n"));

  let pids;
  const pidDeadline = Date.now() + 7_000;
  while (Date.now() < pidDeadline) {
    try {
      pids = JSON.parse(await readFile(pidFile, "utf8"));
      break;
    } catch {}
    if (responseBytes.includes("HTTP/1.1 4") || responseBytes.includes("HTTP/1.1 5")) break;
    await sleep(50);
  }
  assert.ok(pids?.parent && pids?.child, `tool process did not start; raw response=${responseBytes.slice(0, 2000)}`);
  assert.equal(processAlive(pids.parent), true);
  assert.equal(processAlive(pids.child), true);

  socket.destroy();
  const deathDeadline = Date.now() + 7_000;
  while (Date.now() < deathDeadline && (processAlive(pids.parent) || processAlive(pids.child))) {
    await sleep(100);
  }
  assert.equal(processAlive(pids.parent), false, `parent PID ${pids.parent} survived HTTP disconnect`);
  assert.equal(processAlive(pids.child), false, `child PID ${pids.child} survived HTTP disconnect`);

  let aborted;
  let toolStart;
  let toolFinish;
  let toolThrow;
  const telemetryDeadline = Date.now() + 4_000;
  while (Date.now() < telemetryDeadline && (!aborted || (!toolFinish && !toolThrow))) {
    try {
      const usageText = await readFile(path.join(runtimeDir, "run", "http-usage.jsonl"), "utf8");
      const usage = usageText.trim().split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
      aborted = usage.find((event) => event.type === "http_request" && event.method === "POST" && event.path === "/" && event.client_aborted === true);
    } catch {}
    try {
      const toolUsageText = await readFile(path.join(runtimeDir, "run", "tool-usage.jsonl"), "utf8");
      const toolUsage = toolUsageText.trim().split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
      toolStart = toolUsage.find((event) => event.type === "tool_start" && event.tool === "devbox_run_program");
      toolFinish = toolUsage.find((event) => event.type === "tool_finish" && event.invocation_id === toolStart?.invocation_id);
      toolThrow = toolUsage.find((event) => event.type === "tool_throw" && event.invocation_id === toolStart?.invocation_id);
    } catch {}
    if (!aborted || (!toolFinish && !toolThrow)) await sleep(100);
  }
  assert.ok(aborted?.request_id, "HTTP disconnect must be recorded as client_aborted");
  assert.equal(aborted.status_code, null);
  assert.ok(toolStart?.invocation_id, "disconnect tool call must log tool_start");
  assert.ok(toolFinish || toolThrow, "disconnect tool call must close tool telemetry");
  assert.ok(!(toolFinish && toolThrow), "disconnect tool call must have exactly one terminal telemetry event");
  if (toolFinish) {
    assert.equal(toolFinish.is_error, true);
    assert.match(toolFinish.summary || "", /cancel|abort/i);
  } else {
    assert.match(toolThrow.error || "", /client disconnected before the tool result was delivered|cancel|abort/i);
  }
  console.log(JSON.stringify({ ok: true, parentPid: pids.parent, childPid: pids.child, clientAbortedLogged: true, toolTerminalLogged: toolFinish ? "finish" : "throw" }, null, 2));
} catch (error) {
  if (stdout.trim()) console.error(`\n--- Rust MCP stdout ---\n${stdout}`);
  if (stderr.trim()) console.error(`\n--- Rust MCP stderr ---\n${stderr}`);
  throw error;
} finally {
  socket?.destroy();
  if (server.exitCode === null && server.signalCode === null) {
    server.kill();
    await Promise.race([exited.catch(() => null), sleep(5_000)]);
    if (server.exitCode === null && server.signalCode === null) {
      server.kill("SIGKILL");
      await Promise.race([exited.catch(() => null), sleep(1_000)]);
    }
  }
  await rm(runtimeDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}
