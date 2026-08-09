import assert from "node:assert/strict";
import { access, mkdtemp, rm } from "node:fs/promises";
import http from "node:http";
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

const rawRequest = ({ port, method = "GET", pathname = "/", headers = {} }) => new Promise((resolve, reject) => {
  const request = http.request({ hostname: "127.0.0.1", port, method, path: pathname, headers }, (response) => {
    const chunks = [];
    response.on("data", (chunk) => chunks.push(chunk));
    response.on("end", () => resolve({
      status: response.statusCode,
      headers: response.headers,
      body: Buffer.concat(chunks).toString("utf8"),
    }));
  });
  request.once("error", reject);
  request.end();
});

const startServer = async ({ authMode }) => {
  const port = await reservePort();
  const runtimeDir = await mkdtemp(path.join(os.tmpdir(), `devbox-rust-gateway-${authMode}-`));
  const publicBaseUrl = "https://devbox.example";
  let stdout = "";
  let stderr = "";
  const child = spawn(binaryPath, [], {
    cwd: projectRoot,
    env: {
      ...process.env,
      DEVBOX_PROJECT_ROOT: runtimeDir,
      HOST_WORKSPACE_PATH: projectRoot,
      HOST: "127.0.0.1",
      PORT: String(port),
      MCP_AUTH_MODE: authMode,
      PUBLIC_BASE_URL: publicBaseUrl,
      OAUTH_STATE_FILE_PATH: path.join(runtimeDir, "oauth-state.json"),
      DEVBOX_RUNTIME_MODE: "host",
      ENABLE_HOST_EXEC: "true",
      ENABLE_GATEWAY_BRIDGE: "true",
      GATEWAY_BRIDGE_ORIGINS: "https://chatgpt.com,https://chat.openai.com",
      HOST_DEFAULT_WORKDIR: projectRoot,
      MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
      MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "execution-slots"),
    },
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout = `${stdout}${chunk}`.slice(-16_000); });
  child.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-16_000); });
  const exited = new Promise((resolve, reject) => {
    child.once("error", reject);
    child.once("exit", (code, signal) => resolve({ code, signal }));
  });
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const early = await Promise.race([
      exited.then((result) => ({ result })),
      new Promise((resolve) => setTimeout(() => resolve(null), 100)),
    ]);
    if (early) throw new Error(`gateway smoke server exited early: ${JSON.stringify(early.result)}\n${stdout}\n${stderr}`);
    try {
      const health = await rawRequest({ port, headers: { Host: `127.0.0.1:${port}` }, pathname: "/healthz" });
      if (health.status === 200 && health.body === "ok") break;
    } catch {}
  }
  return {
    port,
    publicBaseUrl,
    runtimeDir,
    child,
    exited,
    logs: () => ({ stdout, stderr }),
  };
};

const stopServer = async (server) => {
  if (server.child.exitCode === null && server.child.signalCode === null) {
    server.child.kill();
    await Promise.race([server.exited, new Promise((resolve) => setTimeout(resolve, 5_000))]);
    if (server.child.exitCode === null && server.child.signalCode === null) server.child.kill("SIGKILL");
  }
  await rm(server.runtimeDir, { recursive: true, force: true });
};

await access(binaryPath);
const noAuth = await startServer({ authMode: "none" });
try {
  const invalidHost = await rawRequest({
    port: noAuth.port,
    headers: { Host: "evil.example" },
  });
  assert.equal(invalidHost.status, 403);
  assert.equal(JSON.parse(invalidHost.body).error?.message, "Invalid Host: evil.example");

  const localHost = `127.0.0.1:${noAuth.port}`;
  const localRoot = await rawRequest({
    port: noAuth.port,
    headers: { Host: localHost, Origin: "https://chatgpt.com" },
  });
  assert.equal(localRoot.status, 200);
  assert.equal(localRoot.headers["access-control-allow-origin"], "https://chatgpt.com");
  const localMetadata = JSON.parse(localRoot.body);
  assert.equal(localMetadata.local_base_url, `http://${localHost}`);
  assert.equal(localMetadata.root_mcp_url, noAuth.publicBaseUrl);
  assert.equal(localMetadata.mcp_url, `${noAuth.publicBaseUrl}/mcp`);
  assert.deepEqual(localMetadata.gateway_bridge, {
    enabled: true,
    origins: ["https://chatgpt.com", "https://chat.openai.com"],
    private_network_access: true,
  });
  assert.ok(localMetadata.runtime);
  assert.ok(localMetadata.devbox);

  const remoteRoot = await rawRequest({
    port: noAuth.port,
    headers: {
      Host: "devbox.example",
      Origin: "https://chatgpt.com",
      "X-Forwarded-For": "203.0.113.8, 127.0.0.1",
      "X-Forwarded-Proto": "https",
    },
  });
  assert.equal(remoteRoot.status, 200);
  assert.equal(remoteRoot.headers["access-control-allow-origin"], undefined);
  const remoteMetadata = JSON.parse(remoteRoot.body);
  assert.equal(remoteMetadata.local_base_url, null);
  assert.deepEqual(remoteMetadata.gateway_bridge, {
    enabled: false,
    origins: [],
    private_network_access: false,
  });
  assert.ok(remoteMetadata.runtime, "auth=none JS root still exposes runtime to remote requests");
  assert.ok(remoteMetadata.devbox, "auth=none JS root still exposes devbox to remote requests");

  const allowedPreflight = await rawRequest({
    port: noAuth.port,
    method: "OPTIONS",
    headers: {
      Host: localHost,
      Origin: "https://chatgpt.com",
      "Access-Control-Request-Method": "POST",
      "Access-Control-Request-Headers": "authorization, x-smoke",
      "Access-Control-Request-Private-Network": "true",
    },
  });
  assert.equal(allowedPreflight.status, 204);
  assert.equal(allowedPreflight.headers["access-control-allow-origin"], "https://chatgpt.com");
  assert.equal(allowedPreflight.headers["access-control-allow-methods"], "DELETE, GET, HEAD, OPTIONS, POST");
  assert.equal(allowedPreflight.headers["access-control-allow-headers"], "authorization, x-smoke");
  assert.equal(allowedPreflight.headers["access-control-expose-headers"], "mcp-session-id");
  assert.equal(allowedPreflight.headers["access-control-max-age"], "600");
  assert.equal(allowedPreflight.headers["access-control-allow-private-network"], "true");
  for (const field of ["Origin", "Access-Control-Request-Method", "Access-Control-Request-Headers", "Access-Control-Request-Private-Network"]) {
    assert.ok((allowedPreflight.headers.vary || "").split(",").map((value) => value.trim()).includes(field));
  }

  const deniedOrigin = await rawRequest({
    port: noAuth.port,
    method: "OPTIONS",
    headers: { Host: localHost, Origin: "https://evil.example", "Access-Control-Request-Method": "POST" },
  });
  assert.equal(deniedOrigin.status, 403);
  assert.equal(JSON.parse(deniedOrigin.body).error, "Origin is not allowed for the local ChatGPT gateway bridge.");

  const remotePreflight = await rawRequest({
    port: noAuth.port,
    method: "OPTIONS",
    headers: {
      Host: "devbox.example",
      Origin: "https://chatgpt.com",
      "X-Forwarded-For": "203.0.113.8",
      "Access-Control-Request-Method": "POST",
    },
  });
  assert.equal(remotePreflight.status, 405);
  assert.equal(remotePreflight.headers["access-control-allow-origin"], undefined);
} catch (error) {
  const logs = noAuth.logs();
  if (logs.stdout.trim()) console.error(`\n--- gateway no-auth stdout ---\n${logs.stdout}`);
  if (logs.stderr.trim()) console.error(`\n--- gateway no-auth stderr ---\n${logs.stderr}`);
  throw error;
} finally {
  await stopServer(noAuth);
}

const oauth = await startServer({ authMode: "demo-oauth" });
try {
  const remoteRoot = await rawRequest({
    port: oauth.port,
    headers: {
      Host: "devbox.example",
      "X-Forwarded-For": "203.0.113.8",
      "X-Forwarded-Proto": "https",
    },
  });
  assert.equal(remoteRoot.status, 200);
  const metadata = JSON.parse(remoteRoot.body);
  assert.equal(metadata.auth_mode, "demo-oauth");
  assert.equal(metadata.local_base_url, null);
  assert.equal(metadata.root_mcp_url, oauth.publicBaseUrl);
  assert.equal(metadata.runtime, undefined);
  assert.equal(metadata.devbox, undefined);
  assert.deepEqual(metadata.gateway_bridge, {
    enabled: false,
    origins: [],
    private_network_access: false,
  });

  const oauthPreflight = await rawRequest({
    port: oauth.port,
    method: "OPTIONS",
    headers: { Host: `127.0.0.1:${oauth.port}`, Origin: "https://chatgpt.com", "Access-Control-Request-Method": "POST" },
  });
  assert.equal(oauthPreflight.status, 405);
} catch (error) {
  const logs = oauth.logs();
  if (logs.stdout.trim()) console.error(`\n--- gateway oauth stdout ---\n${logs.stdout}`);
  if (logs.stderr.trim()) console.error(`\n--- gateway oauth stderr ---\n${logs.stderr}`);
  throw error;
} finally {
  await stopServer(oauth);
}

console.log(JSON.stringify({ ok: true, hostValidation: true, bridgeCors: true, privateNetworkAccess: true, remoteOauthRedaction: true }, null, 2));
