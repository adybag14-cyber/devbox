import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, writeFile, rm} from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const binary = process.env.DEVBOX_CPP_TRANSPORT_TEST_BINARY || path.join(repo, '.cpp-build/windows/cpp-mcp/Release/devbox-transport-tests.exe');
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-sdk-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const child = spawn(binary, ['--serve', root], {cwd: repo, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
let output = '';
for (const stream of [child.stdout, child.stderr]) stream.on('data', data => { output = (output + data.toString()).slice(-16000); });
const exited = new Promise((resolve, reject) => { child.once('exit', resolve); child.once('error', reject); });
let client;
try {
  let ready;
  const deadline = Date.now() + 20000;
  while (!ready) {
    assert(Date.now() < deadline, `Fixture did not start: ${output}`);
    assert.equal(child.exitCode, null, output);
    try { ready = JSON.parse(await readFile(path.join(root, 'ready.json'), 'utf8')); } catch {}
    if (!ready) await delay(25);
  }
  assert.equal(ready.pid, child.pid, 'owned fixture PID');
  const base = `http://127.0.0.1:${ready.port}`;
  const transport = new StreamableHTTPClientTransport(new URL(`${base}/mcp`));
  client = new Client({name: 'cpp-sdk-smoke', version: '1'}, {capabilities: {}});
  await client.connect(transport);
  const listed = await client.listTools();
  assert.deepEqual(listed.tools.map(tool => tool.name), ['devbox_wait', 'host_exec']);
  const result = await client.callTool({name: 'devbox_wait', arguments: {delay_ms: 1200}});
  assert.equal(result.isError, false);
  assert.equal(result.structuredContent.summary, 'fixture complete');
  const abort = new AbortController();
  const pending = client.callTool({name: 'devbox_wait', arguments: {delay_ms: 10000}}, undefined, {signal: abort.signal});
  const cancelled = assert.rejects(pending);
  await delay(75);
  abort.abort(new Error('SDK cancellation fixture'));
  await cancelled;
  const fanout = Array.from({length: 24}, () => client.callTool({name: 'devbox_wait', arguments: {delay_ms: 250}}));
  const started = performance.now();
  const health = await fetch(`${base}/healthz`, {signal: AbortSignal.timeout(1000)});
  assert.equal(await health.text(), 'ok');
  const healthMs = performance.now() - started;
  assert(healthMs < 1000, `health delayed by passive waits: ${healthMs}ms`);
  assert((await Promise.all(fanout)).every(value => value.isError === false));
  await client.close(); client = undefined;
  await writeFile(path.join(root, 'stop'), 'stop');
  await Promise.race([exited, delay(5000)]);
  assert.equal(child.exitCode, 0, `Fixture failed to stop: ${output}`);
  console.log(JSON.stringify({ok: true, sdk: '1.30.0', handshake: true, sseHeartbeat: true, cancellation: true, concurrentPassiveWaits: 24, healthMs}, null, 2));
} finally {
  await client?.close().catch(() => {});
  if (child.exitCode === null && child.signalCode === null) {
    await writeFile(path.join(root, 'stop'), 'stop');
    await Promise.race([exited, delay(5000)]);
    if (child.exitCode === null && child.signalCode === null) { child.kill(); await Promise.race([exited, delay(5000)]); }
  }
  assert(child.exitCode !== null || child.signalCode !== null, 'owned fixture process stopped');
  await rm(root, {recursive: true, force: true, maxRetries: 20, retryDelay: 100});
}
