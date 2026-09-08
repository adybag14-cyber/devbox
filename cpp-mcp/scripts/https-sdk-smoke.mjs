import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const binary = process.env.DEVBOX_MCP_TEST_BINARY;
assert(binary && path.isAbsolute(binary), 'Supply the native C++ test binary');
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-https-'));
const jwks = 'https://www.googleapis.com/oauth2/v3/certs';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const invalidCa = path.join(root, 'invalid-ca.pem');
await writeFile(invalidCa, 'This is deliberately not a trusted certificate bundle.\n');
const binarySha256 = createHash('sha256').update(await readFile(binary)).digest('hex');
const outputs = [];
try {
  for (const invalid of [false, true]) {
    const port = await new Promise((resolve, reject) => {
      const socket = net.createServer(); socket.once('error', reject);
      socket.listen(0, '127.0.0.1', () => { const value = socket.address().port; socket.close(() => resolve(value)); });
    });
    const base = `http://127.0.0.1:${port}/`;
    const state = path.join(root, invalid ? 'invalid-ca' : 'system-ca');
    const env = { ...process.env, DEVBOX_PROJECT_ROOT: state, HOST: '127.0.0.1', PORT: String(port),
      MCP_AUTH_MODE: 'cloudflare-access', PUBLIC_BASE_URL: base, DEVBOX_RUNTIME_MODE: 'host',
      DEVBOX_AUTO_START: 'false', ENABLE_HOST_EXEC: 'false',
      CLOUDFLARE_ACCESS_TEAM_DOMAIN: 'https://fixture.cloudflareaccess.com',
      CLOUDFLARE_ACCESS_AUD: 'https-fixture', CLOUDFLARE_ACCESS_JWKS_URL: jwks,
      OAUTH_STATE_FILE_PATH: path.join(state, 'oauth.json'), MCP_JOBS_ROOT: path.join(state, 'jobs'),
      MCP_EXEC_SLOT_ROOT: path.join(state, 'slots') };
    delete env.CURL_CA_BUNDLE; delete env.SSL_CERT_FILE; delete env.SSL_CERT_DIR;
    if (invalid) env.CURL_CA_BUNDLE = invalidCa;
    const child = spawn(binary, [], { env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { output = (output + bytes).slice(-12000); });
    const exited = new Promise((resolve, reject) => { child.once('exit', resolve); child.once('error', reject); });
    const request = (url, options = {}) => fetch(url, { signal: AbortSignal.timeout(15000), ...options });
    try {
      const deadline = Date.now() + 20000;
      for (;;) {
        assert(Date.now() < deadline, output); assert.equal(child.exitCode, null, output);
        try { if ((await request(new URL('healthz', base))).ok) break; } catch {}
        await delay(50);
      }
      const registration = await request(new URL('register', base), { method: 'POST',
        headers: { 'content-type': 'application/json' }, body: JSON.stringify({
          redirect_uris: ['http://127.0.0.1:19000/callback'], token_endpoint_auth_method: 'none',
          response_types: ['code'], grant_types: ['authorization_code', 'refresh_token'],
        }) });
      assert.equal(registration.status, 201);
      const client = await registration.json();
      const authorize = new URL('authorize', base);
      authorize.search = new URLSearchParams({ client_id: client.client_id,
        redirect_uri: 'http://127.0.0.1:19000/callback', response_type: 'code',
        code_challenge: createHash('sha256').update('non-secret-https-fixture').digest('base64url'),
        code_challenge_method: 'S256', scope: 'mcp:tools', resource: base, state: 'https-fixture' });
      const header = Buffer.from(JSON.stringify({ alg: 'RS256', kid: `devbox-fixture-${randomUUID()}` })).toString('base64url');
      const response = await request(authorize, { redirect: 'manual',
        headers: { 'cf-access-jwt-assertion': `${header}.e30.AA` } });
      assert.equal(response.status, 302);
      const location = new URL(response.headers.get('location'));
      assert.equal(location.searchParams.get('error'), 'server_error');
      const message = location.searchParams.get('error_description');
      if (invalid) {
        assert.doesNotMatch(message, /unknown kid/);
        assert.match(message, /certificate|SSL|CA|cert/i);
      } else {
        // This error is emitted only after a verified HTTPS response is parsed
        // as a JWKS document. The non-secret fake JWT never authenticates.
        assert.match(message, /verification failed: unknown kid$/);
      }
      outputs.push(invalid ? 'explicit invalid CA rejected' : 'system CA verified native HTTPS and parsed JWKS');
    } finally {
      if (child.exitCode === null && child.signalCode === null) {
        child.kill(); await Promise.race([exited, delay(5000)]);
        if (child.exitCode === null && child.signalCode === null) { child.kill('SIGKILL'); await exited; }
      }
    }
  }
  console.log(JSON.stringify({ ok: true, binarySha256, jwks, checks: outputs }));
} finally { await rm(root, { recursive: true, force: true }); }
