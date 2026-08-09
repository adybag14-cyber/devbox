import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { access, mkdtemp, readFile, rm } from "node:fs/promises";
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
const form = (values) => new URLSearchParams(Object.entries(values).filter(([, value]) => value !== undefined));
const pkce = (verifier) => createHash("sha256").update(verifier).digest("base64url");

await access(binaryPath);
const port = await reservePort();
const baseUrl = new URL(`http://127.0.0.1:${port}/`);
const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-oauth-"));
const statePath = path.join(runtimeDir, "oauth-state.json");
const server = spawn(binaryPath, [], {
  cwd: projectRoot,
  env: {
    ...process.env,
    DEVBOX_PROJECT_ROOT: projectRoot,
    HOST: "127.0.0.1",
    PORT: String(port),
    MCP_AUTH_MODE: "demo-oauth",
    PUBLIC_BASE_URL: baseUrl.toString().replace(/\/$/, ""),
    OAUTH_STATE_FILE_PATH: statePath,
    DEVBOX_RUNTIME_MODE: "host",
    ENABLE_HOST_EXEC: "true",
    HOST_DEFAULT_WORKDIR: projectRoot,
    MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
    MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "execution-slots"),
  },
  stdio: ["ignore", "pipe", "pipe"],
  windowsHide: true,
});
let stdout = "";
let stderr = "";
server.stdout.setEncoding("utf8");
server.stderr.setEncoding("utf8");
server.stdout.on("data", (chunk) => { stdout = `${stdout}${chunk}`.slice(-32_000); });
server.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-32_000); });
const exited = new Promise((resolve, reject) => {
  server.once("error", reject);
  server.once("exit", (code, signal) => resolve({ code, signal }));
});

const waitForHealth = async () => {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const early = await Promise.race([exited.then((result) => ({ result })), new Promise((resolve) => setTimeout(() => resolve(null), 100))]);
    if (early) throw new Error(`OAuth smoke server exited early: ${JSON.stringify(early.result)}`);
    try {
      const response = await fetch(new URL("healthz", baseUrl));
      if (response.ok && await response.text() === "ok") return;
    } catch {}
  }
  throw new Error("OAuth smoke server did not become healthy");
};

let client;
try {
  await waitForHealth();

  const rootMetadata = await (await fetch(baseUrl)).json();
  assert.equal(rootMetadata.auth_mode, "demo-oauth");
  assert.equal(rootMetadata.oauth?.issuer, baseUrl.toString());
  assert.equal(rootMetadata.oauth?.resourceMetadataUrl, new URL(".well-known/oauth-protected-resource", baseUrl).toString());

  const metadata = await (await fetch(new URL(".well-known/oauth-authorization-server", baseUrl))).json();
  assert.equal(metadata.issuer, baseUrl.toString());
  assert.equal(metadata.authorization_endpoint, new URL("authorize", baseUrl).toString());
  assert.equal(metadata.token_endpoint, new URL("token", baseUrl).toString());
  assert.equal(metadata.registration_endpoint, new URL("register", baseUrl).toString());
  assert.equal(metadata.revocation_endpoint, new URL("revoke", baseUrl).toString());
  assert.deepEqual(metadata.code_challenge_methods_supported, ["S256"]);

  const rootResource = await (await fetch(new URL(".well-known/oauth-protected-resource", baseUrl))).json();
  const legacyResource = await (await fetch(new URL(".well-known/oauth-protected-resource/mcp", baseUrl))).json();
  assert.equal(rootResource.resource, baseUrl.toString());
  assert.equal(legacyResource.resource, new URL("mcp", baseUrl).toString());
  assert.deepEqual(rootResource.scopes_supported, ["mcp:tools"]);

  const unauthorized = await fetch(baseUrl, {
    method: "POST",
    headers: { Accept: "application/json, text/event-stream", "Content-Type": "application/json" },
    body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "oauth-smoke", version: "1" } } }),
  });
  assert.equal(unauthorized.status, 401);
  assert.match(unauthorized.headers.get("www-authenticate") || "", /error="invalid_token"/);
  assert.match(unauthorized.headers.get("www-authenticate") || "", /\.well-known\/oauth-protected-resource/);

  const unsafeRegistration = await fetch(new URL("register", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ redirect_uris: ["javascript:alert(1)"] }),
  });
  assert.equal(unsafeRegistration.status, 400);
  assert.equal((await unsafeRegistration.json()).error, "invalid_client_metadata");

  const registeredRedirect = "http://127.0.0.1:19000/callback";
  const relaxedRedirect = "http://127.0.0.1:19123/callback";
  const actualRedirect = registeredRedirect;
  const registration = await fetch(new URL("register", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      redirect_uris: [registeredRedirect],
      token_endpoint_auth_method: "none",
      grant_types: ["authorization_code", "refresh_token"],
      response_types: ["code"],
      client_name: "Rust OAuth Smoke",
    }),
  });
  assert.equal(registration.status, 201);
  const registered = await registration.json();
  assert.ok(registered.client_id);
  assert.equal(registered.client_secret, undefined);

  const verifier = "rust-oauth-smoke-verifier-abcdefghijklmnopqrstuvwxyz-0123456789";
  const relaxedUrl = new URL("authorize", baseUrl);
  relaxedUrl.search = new URLSearchParams({
    client_id: registered.client_id,
    redirect_uri: relaxedRedirect,
    response_type: "code",
    code_challenge: pkce(verifier),
    code_challenge_method: "S256",
    state: "relaxed-port-state",
  }).toString();
  const relaxed = await fetch(relaxedUrl, { redirect: "manual" });
  assert.equal(relaxed.status, 302);
  const relaxedError = new URL(relaxed.headers.get("location"));
  assert.equal(relaxedError.searchParams.get("error"), "invalid_request");
  assert.equal(relaxedError.searchParams.get("error_description"), "Unregistered redirect_uri.");

  const authorizeUrl = new URL("authorize", baseUrl);
  authorizeUrl.search = new URLSearchParams({
    client_id: registered.client_id,
    redirect_uri: actualRedirect,
    response_type: "code",
    code_challenge: pkce(verifier),
    code_challenge_method: "S256",
    scope: "mcp:tools",
    state: "smoke-state",
    resource: baseUrl.toString(),
  }).toString();
  const authorize = await fetch(authorizeUrl, { redirect: "manual" });
  assert.equal(authorize.status, 302);
  const redirect = new URL(authorize.headers.get("location"));
  assert.equal(redirect.origin + redirect.pathname, new URL(actualRedirect).origin + new URL(actualRedirect).pathname);
  assert.equal(redirect.searchParams.get("state"), "smoke-state");
  const code = redirect.searchParams.get("code");
  assert.ok(code);

  const tokenResponse = await fetch(new URL("token", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: form({
      grant_type: "authorization_code",
      client_id: registered.client_id,
      code,
      code_verifier: verifier,
      redirect_uri: actualRedirect,
      resource: baseUrl.toString(),
    }),
  });
  assert.equal(tokenResponse.status, 200);
  const tokens = await tokenResponse.json();
  assert.ok(tokens.access_token);
  assert.ok(tokens.refresh_token);
  assert.equal(tokens.token_type, "bearer");
  assert.equal(tokens.scope, "mcp:tools");

  client = new Client({ name: "rust-oauth-smoke", version: "1.0.0" });
  const transport = new StreamableHTTPClientTransport(baseUrl, {
    requestInit: { headers: { Authorization: `Bearer ${tokens.access_token}` } },
  });
  await client.connect(transport);
  const tools = await client.listTools();
  assert.equal(tools.tools.length, 37);
  await client.close();
  client = undefined;

  const refreshedResponse = await fetch(new URL("token", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: form({ grant_type: "refresh_token", client_id: registered.client_id, refresh_token: tokens.refresh_token }),
  });
  assert.equal(refreshedResponse.status, 200);
  const refreshed = await refreshedResponse.json();
  assert.ok(refreshed.access_token);

  const revoke = await fetch(new URL("revoke", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: form({ client_id: registered.client_id, token: refreshed.access_token, token_type_hint: "access_token" }),
  });
  assert.equal(revoke.status, 200);
  assert.deepEqual(await revoke.json(), {});

  const revoked = await fetch(baseUrl, {
    method: "POST",
    headers: { Authorization: `Bearer ${refreshed.access_token}`, Accept: "application/json, text/event-stream", "Content-Type": "application/json" },
    body: JSON.stringify({ jsonrpc: "2.0", id: 2, method: "ping" }),
  });
  assert.equal(revoked.status, 401);
  assert.match((await revoked.json()).error || "", /invalid_token/);

  const persisted = JSON.parse(await readFile(statePath, "utf8"));
  assert.ok(Array.isArray(persisted.clients) && persisted.clients.length === 1);
  assert.ok(Array.isArray(persisted.accessTokens));
  assert.ok(Array.isArray(persisted.refreshTokens));

  console.log(JSON.stringify({ ok: true, authMode: "demo-oauth", tools: tools.tools.length, persistedClients: persisted.clients.length }, null, 2));
} catch (error) {
  if (stdout.trim()) console.error(`\n--- Rust MCP stdout ---\n${stdout}`);
  if (stderr.trim()) console.error(`\n--- Rust MCP stderr ---\n${stderr}`);
  throw error;
} finally {
  if (client) await client.close().catch(() => {});
  if (server.exitCode === null && server.signalCode === null) {
    server.kill();
    await Promise.race([exited, new Promise((resolve) => setTimeout(resolve, 5_000))]);
    if (server.exitCode === null && server.signalCode === null) server.kill("SIGKILL");
  }
  await rm(runtimeDir, { recursive: true, force: true });
}
