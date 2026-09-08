import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';

// Both executables are development artifacts. Never attach to an existing server.
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const binaries = {
  rust: process.env.DEVBOX_RUST_REFERENCE_BINARY,
  cpp: process.env.DEVBOX_MCP_TEST_BINARY,
};
for (const [name, binary] of Object.entries(binaries)) {
  assert(binary && path.isAbsolute(binary), `Supply an absolute ${name} development binary path`);
  await access(binary);
}
const identities = Object.fromEntries(Object.entries(binaries).map(([name, binary]) =>
  [name, JSON.parse(execFileSync(binary, ['--build-info'], { encoding: 'utf8', windowsHide: true, timeout: 10000 }))]));
const sha = value => createHash('sha256').update(value).digest('hex');
const base64 = value => Buffer.from(value).toString('base64');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const terminal = new Set(['succeeded', 'failed', 'cancelled', 'timed_out', 'interrupted']);
const checks = [];

async function reservePort() {
  return new Promise((resolve, reject) => {
    const listener = net.createServer();
    listener.once('error', reject);
    listener.listen(0, '127.0.0.1', () => {
      const port = listener.address().port;
      listener.close(error => error ? reject(error) : resolve(port));
    });
  });
}

async function scenario(from, to, auth) {
  const root = await mkdtemp(path.join(os.tmpdir(), `devbox-crossover-${from}-${to}-`));
  const port = await reservePort();
  const url = new URL(`http://127.0.0.1:${port}/`);
  const ownedJobs = new Set();
  const env = {
    ...process.env, DEVBOX_PROJECT_ROOT: root, HOST: '127.0.0.1', PORT: String(port),
    MCP_AUTH_MODE: auth, PUBLIC_BASE_URL: auth === 'none' ? '' : url.toString(),
    OAUTH_STATE_FILE_PATH: path.join(root, 'oauth-state.json'),
    DEVBOX_RUNTIME_MODE: 'host', ENABLE_HOST_EXEC: 'true', NODE_EXE: process.execPath,
    HOST_WORKSPACE_PATH: root, HOST_DEFAULT_WORKDIR: root, DEVBOX_WORKSPACE_PATH: root,
    MCP_JOBS_ROOT: path.join(root, 'jobs'), MCP_EXEC_SLOT_ROOT: path.join(root, 'slots'),
    MCP_JOB_MAX_ACTIVE_RUNNERS: '3', MCP_JOB_MAX_RUNNERS_PER_TASK: '1',
    MCP_EXEC_MAX_CONCURRENT: '6', MCP_EXEC_FOREGROUND_RESERVE: '1',
    MCP_EXEC_HEAVY_CAPACITY: '5', MCP_EXEC_HEAVY_WEIGHT: '5',
  };
  let server, exited, client, logs = '', currentKind = from, token;
  async function stop() {
    await client?.close().catch(() => {}); client = undefined;
    if (!server || server.exitCode !== null || server.signalCode !== null) return;
    // This handle is the exact server spawned below. Detached runners must survive.
    server.kill('SIGKILL');
    await Promise.race([exited, sleep(5000)]);
    assert(server.exitCode !== null || server.signalCode !== null, `owned server ${server.pid} stopped`);
  }
  async function start(kind) {
    currentKind = kind;
    logs = '';
    server = spawn(binaries[kind], [], { cwd: repo, env, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    exited = new Promise((resolve, reject) => { server.once('error', reject); server.once('exit', resolve); });
    for (const stream of [server.stdout, server.stderr]) {
      stream.setEncoding('utf8'); stream.on('data', chunk => { logs = (logs + chunk).slice(-12000); });
    }
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
      assert(server.exitCode === null && server.signalCode === null, `${kind} startup failed: ${logs}`);
      try {
        const response = await fetch(new URL('readyz', url), { signal: AbortSignal.timeout(500) });
        if (response.ok) return;
      } catch {}
      await sleep(100);
    }
    throw new Error(`${kind} readiness deadline: ${logs}`);
  }
  async function connect(accessToken = token) {
    await client?.close().catch(() => {});
    client = new Client({ name: 'cpp-runtime-crossover', version: '1' });
    await client.connect(new StreamableHTTPClientTransport(url, {
      requestInit: { headers: accessToken ? { Authorization: `Bearer ${accessToken}` } : {} },
    }));
    assert.equal((await client.listTools()).tools.length, 45);
  }
  async function call(name, args = {}, failure = false) {
    const result = await client.callTool({ name, arguments: args }, undefined, { timeout: 15000 });
    assert.equal(result.isError ?? false, failure, `${name}: ${JSON.stringify(result.structuredContent)}`);
    if (failure) return result;
    assert.equal(result.structuredContent?.ok, true);
    return result.structuredContent.data;
  }
  async function statusUntil(id, predicate) {
    const deadline = Date.now() + 12000;
    let status;
    do {
      status = await call('devbox_job_status', { job_id: id });
      if (predicate(status)) return status;
      await sleep(100);
    } while (Date.now() < deadline);
    throw new Error(`Job deadline: ${JSON.stringify(status)}`);
  }
  async function formPost(route, values, expectedStatus = 200) {
    const response = await fetch(new URL(route, url), {
      method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams(values), signal: AbortSignal.timeout(5000),
    });
    assert.equal(response.status, expectedStatus, `${currentKind} ${route} status`);
    return response.json();
  }
  async function check(name, work) {
    const started = Date.now(); await work();
    const result = { direction: `${from}->${to}`, name, ok: true, durationMs: Date.now() - started };
    checks.push(result); console.log(JSON.stringify(result));
  }
  try {
    await start(from);
    if (auth === 'none') {
      await connect();
      const file = path.join(root, 'atomic.bin');
      const append = { path: file, content_base64: base64('second'), append: true,
        expected_file_sha256: sha('first'), expected_offset_bytes: 5 };
      const task = { task_id: 'checkpoint', expected_revision: 0, state: { phase: 'running', nested: { value: 1 } } };
      const counter = path.join(root, 'counter');
      const once = { task_id: 'once', operation_id: 'build', program: 'node',
        args: ['-e', "const fs=require('fs'),p=process.argv[1];fs.appendFileSync(p,'once\\n');console.log('completed')", counter], working_dir: root };
      let finished, active, before;
      const heavy = { task_id: 'active', operation_id: 'run', program: 'node',
        args: ['-e', "console.log('ready');setInterval(()=>{},1000)"], working_dir: root,
        resource_class: 'heavy', timeout_seconds: 120 };
      await check('create durable file, task, receipt and live weighted runner', async () => {
        await call('devbox_write_file_atomic', { path: file, content_base64: base64('first'), expected_file_sha256: 'missing' });
        await call('devbox_write_file_atomic', append);
        assert.equal((await call('devbox_task_put', task)).record.revision, 1);
        finished = await call('devbox_job_submit', once); ownedJobs.add(finished.id);
        assert.equal((await statusUntil(finished.id, s => terminal.has(s.status))).status, 'succeeded');
        active = await call('devbox_job_submit', heavy); ownedJobs.add(active.id);
        before = await statusUntil(active.id, s => s.status === 'running' && s.childPid && s.executionSlots?.length === 5);
        assert.equal(before.executionWeight, 5);
      });
      await stop(); await start(to); await connect();
      await check('recover live runner identity, weighted slots and operation receipts', async () => {
        const recovered = await call('devbox_job_status', { job_id: active.id });
        assert.equal(recovered.status, 'running'); assert.equal(recovered.runnerAlive, true);
        assert.equal(recovered.childPid, before.childPid); assert.equal(recovered.runnerPid, before.runnerPid);
        assert.deepEqual(recovered.executionSlots, before.executionSlots);
        const execution = (await call('devbox_status')).execution;
        assert.equal(execution.occupied_slots.length, 5);
        assert(execution.occupied_slots.every(slot => slot.weight === 5 && slot.resourceClass === 'heavy'));
        assert.equal((await call('devbox_job_submit', heavy)).id, active.id);
        assert.equal((await call('devbox_job_submit', heavy)).replayed, true);
        assert.equal((await call('devbox_job_submit', once)).replayed, true);
        assert.equal(await readFile(counter, 'utf8'), 'once\n');
        await call('devbox_job_submit', { ...once, args: ['--version'] }, true);
      });
      await check('replay append and task CAS across runtime switch', async () => {
        assert.equal((await call('devbox_write_file_atomic', append)).replayed, true);
        assert.equal(await readFile(file, 'utf8'), 'firstsecond');
        assert.deepEqual((await call('devbox_task_get', { task_id: task.task_id })).record.state, task.state);
        assert.equal((await call('devbox_task_put', task)).replayed, true);
        await call('devbox_task_put', { ...task, state: { wrong: true } }, true);
        assert.equal((await call('devbox_task_put', { ...task, expected_revision: 1, state: { phase: 'complete' } })).record.revision, 2);
      });
      await check('cancel original runner and child through replacement runtime', async () => {
        await call('devbox_job_cancel', { job_id: active.id });
        const cancelled = await statusUntil(active.id, s => terminal.has(s.status));
        assert.equal(cancelled.status, 'cancelled'); assert.equal(cancelled.runnerAlive, false);
        assert.throws(() => process.kill(before.childPid, 0), { code: 'ESRCH' });
        assert.throws(() => process.kill(before.runnerPid, 0), { code: 'ESRCH' });
        ownedJobs.delete(active.id);
      });
      await check('retained receipts prevent rerun after result deletion', async () => {
        const target = path.resolve(root, 'jobs', finished.id);
        assert(target.startsWith(path.resolve(root, 'jobs') + path.sep));
        await rm(target, { recursive: true, force: true }); ownedJobs.delete(finished.id);
        const replay = await call('devbox_job_submit', once);
        assert.equal(replay.replayed, true); assert.equal(replay.job.status, 'result_expired');
        assert.equal(await readFile(counter, 'utf8'), 'once\n');
      });
    } else {
      const redirect = 'http://127.0.0.1:19000/callback';
      const verifier = 'crossover-pkce-verifier-abcdefghijklmnopqrstuvwxyz-0123456789';
      const registration = await fetch(new URL('register', url), {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ redirect_uris: [redirect], token_endpoint_auth_method: 'none',
          grant_types: ['authorization_code', 'refresh_token'], response_types: ['code'], client_name: 'Crossover fixture' }),
        signal: AbortSignal.timeout(5000),
      });
      assert.equal(registration.status, 201);
      const registered = await registration.json();
      const authorize = new URL('authorize', url);
      authorize.search = new URLSearchParams({ client_id: registered.client_id, redirect_uri: redirect,
        response_type: 'code', code_challenge: createHash('sha256').update(verifier).digest('base64url'),
        code_challenge_method: 'S256', scope: 'mcp:tools', state: 'crossover', resource: url.toString() });
      const response = await fetch(authorize, { redirect: 'manual', signal: AbortSignal.timeout(5000) });
      assert.equal(response.status, 302);
      const code = new URL(response.headers.get('location')).searchParams.get('code');
      assert(code);
      await stop(); await start(to);
      let tokens, refreshed;
      await check('redeem authorization code and client saved by previous runtime', async () => {
        tokens = await formPost('token', { grant_type: 'authorization_code', client_id: registered.client_id,
          code, code_verifier: verifier, redirect_uri: redirect, resource: url.toString() });
        token = tokens.access_token; assert(token && tokens.refresh_token);
        await connect(); await call('devbox_wait', { seconds: 0.05 });
        assert.equal((await formPost('token', { grant_type: 'authorization_code', client_id: registered.client_id,
          code, code_verifier: verifier, redirect_uri: redirect, resource: url.toString() }, 400)).error, 'invalid_grant');
      });
      await stop(); await start(from);
      await check('accept access token and rotate refresh token after rollback', async () => {
        await connect(); await call('devbox_wait', { seconds: 0.05 });
        refreshed = await formPost('token', { grant_type: 'refresh_token', client_id: registered.client_id, refresh_token: tokens.refresh_token });
        assert(refreshed.access_token && refreshed.refresh_token);
        assert.notEqual(refreshed.refresh_token, tokens.refresh_token);
        token = refreshed.access_token;
      });
      await stop(); await start(to);
      await check('persist consumed refresh token and access revocation across switch', async () => {
        await connect(); await call('devbox_wait', { seconds: 0.05 });
        assert.equal((await formPost('token', { grant_type: 'refresh_token', client_id: registered.client_id,
          refresh_token: tokens.refresh_token }, 400)).error, 'invalid_grant');
        await formPost('revoke', { client_id: registered.client_id, token, token_type_hint: 'access_token' });
      });
      await stop(); await start(from);
      await check('rollback continues to reject revoked token', async () => {
        const response = await fetch(url, { method: 'POST', headers: { Authorization: `Bearer ${token}`,
          Accept: 'application/json', 'Content-Type': 'application/json' },
          body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'ping' }), signal: AbortSignal.timeout(5000) });
        assert.equal(response.status, 401);
      });
    }
  } catch (error) {
    console.error(JSON.stringify({ direction: `${from}->${to}`, auth, root, serverLogs: logs }));
    throw error;
  } finally {
    if (ownedJobs.size) {
      if (!server || server.exitCode !== null || server.signalCode !== null) await start(currentKind);
      if (!client) await connect();
      for (const id of ownedJobs) {
        await call('devbox_job_cancel', { job_id: id }).catch(() => {});
        await statusUntil(id, s => terminal.has(s.status) && !s.runnerAlive).catch(() => {});
      }
    }
    await stop();
    await rm(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}

for (const [from, to] of [['rust', 'cpp'], ['cpp', 'rust']]) {
  await scenario(from, to, 'none');
  await scenario(from, to, 'demo-oauth');
}
console.log(JSON.stringify({ ok: true, identities, checks }, null, 2));
