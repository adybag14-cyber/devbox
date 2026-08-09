import assert from "node:assert/strict";
import { access, mkdtemp, rm } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDir, "..", "..");
const binaryPath = path.join(
  projectRoot,
  "rust-mcp",
  "target",
  "debug",
  process.platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp",
);
const smokeClientPath = path.join(scriptDir, "smoke-client.mjs");
const maxLogChars = 32_000;

const appendTail = (current, chunk) => {
  const combined = `${current}${chunk}`;
  return combined.length <= maxLogChars ? combined : combined.slice(-maxLogChars);
};

const reserveLoopbackPort = () =>
  new Promise((resolve, reject) => {
    const server = net.createServer();
    server.unref();
    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      assert.ok(address && typeof address === "object");
      const { port } = address;
      server.close((error) => (error ? reject(error) : resolve(port)));
    });
  });

const waitForHealth = async ({ baseUrl, exited }) => {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const earlyExit = await Promise.race([
      exited.then((result) => ({ exited: true, result })),
      new Promise((resolve) => setTimeout(() => resolve({ exited: false }), 100)),
    ]);
    if (earlyExit.exited) {
      throw new Error(`Rust MCP exited before readiness (code=${earlyExit.result.code}, signal=${earlyExit.result.signal}).`);
    }
    try {
      const response = await fetch(new URL("healthz", baseUrl));
      if (response.ok && (await response.text()) === "ok") return;
    } catch {
      // Startup races are expected; retry until the bounded deadline.
    }
  }
  throw new Error("Rust MCP did not become healthy within 30 seconds.");
};

const runNode = (args, env) =>
  new Promise((resolve, reject) => {
    const child = spawn(process.execPath, args, {
      cwd: projectRoot,
      env,
      stdio: "inherit",
      windowsHide: true,
    });
    child.once("error", reject);
    child.once("exit", (code, signal) => {
      if (code === 0) resolve();
      else reject(new Error(`Node smoke client failed (code=${code}, signal=${signal}).`));
    });
  });

await access(binaryPath);
const port = await reserveLoopbackPort();
const baseUrl = new URL(`http://127.0.0.1:${port}/`);
const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-mcp-runtime-"));
const serverEnv = {
  ...process.env,
  DEVBOX_PROJECT_ROOT: runtimeDir,
  HOST_WORKSPACE_PATH: projectRoot,
  HOST: "127.0.0.1",
  PORT: String(port),
  MCP_AUTH_MODE: "none",
  DEVBOX_RUNTIME_MODE: "host",
  ENABLE_HOST_EXEC: "true",
  HOST_DEFAULT_WORKDIR: projectRoot,
  MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
  MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "execution-slots"),
};

let stdout = "";
let stderr = "";
const server = spawn(binaryPath, [], {
  cwd: projectRoot,
  env: serverEnv,
  stdio: ["ignore", "pipe", "pipe"],
  windowsHide: true,
});
server.stdout.setEncoding("utf8");
server.stderr.setEncoding("utf8");
server.stdout.on("data", (chunk) => { stdout = appendTail(stdout, chunk); });
server.stderr.on("data", (chunk) => { stderr = appendTail(stderr, chunk); });
const exited = new Promise((resolve, reject) => {
  server.once("error", reject);
  server.once("exit", (code, signal) => resolve({ code, signal }));
});

try {
  await waitForHealth({ baseUrl, exited });
  await runNode([smokeClientPath], {
    ...process.env,
    RUST_MCP_URL: baseUrl.toString(),
    RUST_MCP_STATE_ROOT: runtimeDir,
  });
} catch (error) {
  if (stdout.trim()) console.error(`\n--- Rust MCP stdout ---\n${stdout}`);
  if (stderr.trim()) console.error(`\n--- Rust MCP stderr ---\n${stderr}`);
  throw error;
} finally {
  if (server.exitCode === null && server.signalCode === null) {
    server.kill();
    await Promise.race([exited, new Promise((resolve) => setTimeout(resolve, 5_000))]);
    if (server.exitCode === null && server.signalCode === null) server.kill("SIGKILL");
  }
  await rm(runtimeDir, { recursive: true, force: true });
}
