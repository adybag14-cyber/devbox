import assert from "node:assert/strict";
import { createHash, generateKeyPairSync, sign } from "node:crypto";
import { access, mkdtemp, readFile, rm } from "node:fs/promises";
import http from "node:http";
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
const b64json = (value) => Buffer.from(JSON.stringify(value)).toString("base64url");

await access(binaryPath);
const mcpPort = await reservePort();
const jwksPort = await reservePort();
const baseUrl = new URL(`http://127.0.0.1:${mcpPort}/`);
const issuer = `http://127.0.0.1:${jwksPort}`;
const audience = "cf-access-smoke-audience";
const kid = "rust-cloudflare-smoke-key";
const { publicKey, privateKey } = generateKeyPairSync("rsa", { modulusLength: 2048 });
const jwk = publicKey.export({ format: "jwk" });
Object.assign(jwk, { kid, alg: "RS256", use: "sig" });
const signJwt = (overrides = {}) => {
  const now = Math.floor(Date.now() / 1000);
  const payload = {
    iss: issuer,
    aud: audience,
    sub: "cf-user-123",
    email: "cf-user@example.test",
    name: "Cloudflare Smoke User",
    iat: now,
    exp: now + 300,
    ...overrides,
  };
  const encoded = `${b64json({ alg: "RS256", typ: "JWT", kid })}.${b64json(payload)}`;
  return `${encoded}.${sign("RSA-SHA256", Buffer.from(encoded), privateKey).toString("base64url")}`;
};

let jwksRequests = 0;
const jwksServer = http.createServer((request, response) => {
  if (request.url === "/certs") {
    jwksRequests += 1;
    response.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "public, max-age=300" });
    response.end(JSON.stringify({ keys: [jwk] }));
    return;
  }
  response.writeHead(404).end();
});
await new Promise((resolve, reject) => {
  jwksServer.once("error", reject);
  jwksServer.listen(jwksPort, "127.0.0.1", resolve);
});

const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-cf-oauth-"));
const statePath = path.join(runtimeDir, "oauth-state.json");
const server = spawn(binaryPath, [], {
  cwd: projectRoot,
  env: {
    ...process.env,
    DEVBOX_PROJECT_ROOT: runtimeDir,
    HOST: "127.0.0.1",
    PORT: String(mcpPort),
    MCP_AUTH_MODE: "cloudflare-access",
    PUBLIC_BASE_URL: baseUrl.toString().replace(/\/$/, ""),
    OAUTH_STATE_FILE_PATH: statePath,
    CLOUDFLARE_ACCESS_TEAM_DOMAIN: issuer,
    CLOUDFLARE_ACCESS_AUD: audience,
    CLOUDFLARE_ACCESS_JWKS_URL: `${issuer}/certs`,
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
    if (early) throw new Error(`Cloudflare OAuth smoke server exited early: ${JSON.stringify(early.result)}`);
    try {
      const remaining = Math.max(1, deadline - Date.now());
      const response = await fetch(new URL("healthz", baseUrl), {
        signal: AbortSignal.timeout(Math.min(1_000, remaining)),
      });
      if (response.ok && await response.text() === "ok") return;
    } catch {}
  }
  throw new Error("Cloudflare OAuth smoke server did not become healthy");
};

let client;
try {
  await waitForHealth();
  const registration = await fetch(new URL("register", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ redirect_uris: ["http://127.0.0.1:19000/callback"], token_endpoint_auth_method: "none", response_types: ["code"], grant_types: ["authorization_code", "refresh_token"] }),
  });
  assert.equal(registration.status, 201);
  const registered = await registration.json();
  const actualRedirect = "http://127.0.0.1:19000/callback";
  const verifier = "cloudflare-oauth-smoke-verifier-abcdefghijklmnopqrstuvwxyz-012345";
  const authorizeUrl = new URL("authorize", baseUrl);
  authorizeUrl.search = new URLSearchParams({
    client_id: registered.client_id,
    redirect_uri: actualRedirect,
    response_type: "code",
    code_challenge: pkce(verifier),
    code_challenge_method: "S256",
    scope: "mcp:tools",
    state: "cf-state",
    resource: baseUrl.toString(),
  }).toString();

  const missingIdentity = await fetch(authorizeUrl, { redirect: "manual" });
  assert.equal(missingIdentity.status, 302);
  const missingRedirect = new URL(missingIdentity.headers.get("location"));
  assert.equal(missingRedirect.searchParams.get("error"), "invalid_request");
  assert.match(missingRedirect.searchParams.get("error_description") || "", /Cloudflare Access authentication is required/);
  assert.equal(missingRedirect.searchParams.get("state"), "cf-state");

  const badAudience = await fetch(authorizeUrl, { redirect: "manual", headers: { "cf-access-jwt-assertion": signJwt({ aud: "wrong-audience" }) } });
  assert.equal(badAudience.status, 302);
  const badRedirect = new URL(badAudience.headers.get("location"));
  assert.equal(badRedirect.searchParams.get("error"), "server_error");
  assert.match(badRedirect.searchParams.get("error_description") || "", /JWT verification failed/);

  const authorize = await fetch(authorizeUrl, {
    redirect: "manual",
    headers: { "cf-access-jwt-assertion": signJwt(), "cf-access-authenticated-user-email": "header-fallback@example.test" },
  });
  assert.equal(authorize.status, 302);
  const redirect = new URL(authorize.headers.get("location"));
  const code = redirect.searchParams.get("code");
  assert.ok(code);
  assert.equal(redirect.searchParams.get("state"), "cf-state");

  const tokenResponse = await fetch(new URL("token", baseUrl), {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: form({ grant_type: "authorization_code", client_id: registered.client_id, code, code_verifier: verifier, redirect_uri: actualRedirect, resource: baseUrl.toString() }),
  });
  assert.equal(tokenResponse.status, 200);
  const tokens = await tokenResponse.json();
  assert.ok(tokens.access_token);

  client = new Client({ name: "rust-cloudflare-oauth-smoke", version: "1.0.0" });
  const transport = new StreamableHTTPClientTransport(baseUrl, { requestInit: { headers: { Authorization: `Bearer ${tokens.access_token}` } } });
  await client.connect(transport);
  const tools = await client.listTools();
  assert.equal(tools.tools.length, 37);
  await client.close();
  client = undefined;

  const persisted = JSON.parse(await readFile(statePath, "utf8"));
  const accessRecord = persisted.accessTokens.find(([token]) => token === tokens.access_token)?.[1];
  assert.ok(accessRecord?.identity);
  assert.equal(accessRecord.identity.sub, "cf-user-123");
  assert.equal(accessRecord.identity.email, "cf-user@example.test");
  assert.equal(accessRecord.identity.name, "Cloudflare Smoke User");
  assert.equal(accessRecord.identity.iss, issuer);
  assert.equal(jwksRequests, 1, "Cloudflare JWKS should be cached across authorization attempts");

  console.log(JSON.stringify({ ok: true, authMode: "cloudflare-access", tools: tools.tools.length, identitySub: accessRecord.identity.sub }, null, 2));
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
  await new Promise((resolve) => jwksServer.close(resolve));
  await rm(runtimeDir, { recursive: true, force: true });
}
