import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const revision = '2026-07-28';
const meta = { 'io.modelcontextprotocol/protocolVersion': revision,
  'io.modelcontextprotocol/clientInfo': { name: 'wire-revision-test', version: '1' },
  'io.modelcontextprotocol/clientCapabilities': {} };
const rpc = (method, params = {}, headers = {}, version = revision) => ({
  method: 'POST', body: { jsonrpc: '2.0', id: 1, method, params },
  headers: { Accept: 'application/json, text/event-stream', 'Content-Type': 'application/json',
    ...(version ? { 'MCP-Protocol-Version': version } : {}), 'Mcp-Method': method, ...headers },
});
const call = (headers = {}, requestMeta = meta) => rpc('tools/call', {
  name: 'devbox_task_list', arguments: {}, _meta: requestMeta,
}, { 'Mcp-Name': 'devbox_task_list', ...headers });
const cases = [
  ['legacy initialize', rpc('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'test', version: '1' } }, {}, '')],
  ['modern initialize', rpc('initialize', { protocolVersion: revision, capabilities: {}, clientInfo: { name: 'test', version: '1' } })],
  ['initialize mismatch', rpc('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'test', version: '1' } })],
  ['legacy list', rpc('tools/list', {}, {}, '2025-11-25')],
  ['modern list', rpc('tools/list', { _meta: meta })],
  ['modern call', call()],
  ['modern encoded name', call({ 'Mcp-Name': '=?base64?' + Buffer.from('devbox_task_list').toString('base64') + '?=' })],
  ['modern ping', rpc('ping', { _meta: meta })],
  ['modern discovery', rpc('server/discover', { _meta: meta })],
  ['missing metadata', rpc('tools/list')],
  ['missing client info', rpc('tools/list', { _meta: { ...meta, 'io.modelcontextprotocol/clientInfo': undefined } })],
  ['malformed capabilities', rpc('tools/list', { _meta: { ...meta, 'io.modelcontextprotocol/clientCapabilities': [] } })],
  ['missing method header', rpc('tools/list', { _meta: meta }, { 'Mcp-Method': undefined })],
  ['method header mismatch', rpc('tools/list', { _meta: meta }, { 'Mcp-Method': 'tools/call' })],
  ['missing name header', call({ 'Mcp-Name': undefined })],
  ['name header mismatch', call({ 'Mcp-Name': 'devbox_wait' })],
  ['version metadata mismatch', rpc('tools/list', { _meta: { ...meta, 'io.modelcontextprotocol/protocolVersion': '2025-11-25' } })],
  ['metadata without header', rpc('tools/list', { _meta: meta }, {}, '')],
  ['unsupported version', rpc('tools/list', { _meta: { ...meta, 'io.modelcontextprotocol/protocolVersion': '2099-01-01' } }, {}, '2099-01-01')],
  ['unknown method', rpc('invalid/test', { _meta: meta })],
  ['legacy unknown method', rpc('invalid/test', {}, {}, '2025-11-25')],
  ['modern unknown tool', rpc('tools/call', { name: 'invalid_tool', arguments: {}, _meta: meta }, { 'Mcp-Name': 'invalid_tool' })],
  ['legacy unknown tool', rpc('tools/call', { name: 'invalid_tool', arguments: {} }, {}, '2025-11-25')],
  ['modern invalid arguments', rpc('tools/call', { name: 'devbox_wait', arguments: { seconds: 'bad' }, _meta: meta }, { 'Mcp-Name': 'devbox_wait' })],
  ['modern missing arguments', rpc('tools/call', { name: 'devbox_wait', _meta: meta }, { 'Mcp-Name': 'devbox_wait' })],
  ['modern null arguments', rpc('tools/call', { name: 'devbox_task_list', arguments: null, _meta: meta }, { 'Mcp-Name': 'devbox_task_list' })],
  ['malformed base64 name', call({ 'Mcp-Name': '=?base64?%%%?=' })],
  ['modern subscriptions', rpc('subscriptions/listen', { notifications: { toolsListChanged: true }, _meta: meta })],
  ['modern logging', rpc('logging/setLevel', { level: 'debug', _meta: meta })],
  ['modern resources list', rpc('resources/list', { _meta: meta })],
  ['modern prompts list', rpc('prompts/list', { _meta: meta })],
  ['modern templates list', rpc('resources/templates/list', { _meta: meta })],
  ['modern GET stream', { method: 'GET', headers: { Accept: 'text/event-stream', 'MCP-Protocol-Version': revision } }],
  ['legacy GET stream', { method: 'GET', headers: { Accept: 'text/event-stream' } }],
  ['modern DELETE', { method: 'DELETE', headers: { 'MCP-Protocol-Version': revision } }],
];
function normalized(value) {
  if (Array.isArray(value)) return value.map(normalized);
  if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([key, child]) => {
    if (key === 'serverInfo' || key === 'io.modelcontextprotocol/serverInfo') return [key, '<implementation>'];
    if (key === 'tools') return [key, Array.isArray(child) ? child.length : child];
    return [key, normalized(child)];
  }));
  return value;
}
async function probe(binary) {
  assert(binary && path.isAbsolute(binary));
  const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-wire-parity-'));
  const port = await new Promise((resolve, reject) => { const s = net.createServer(); s.once('error', reject);
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => resolve(p)); }); });
  const url = `http://127.0.0.1:${port}/mcp`;
  const child = spawn(binary, [], { cwd: repo, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], env: {
    ...process.env, DEVBOX_PROJECT_ROOT: root, HOST: '127.0.0.1', PORT: String(port), MCP_AUTH_MODE: 'none',
    DEVBOX_RUNTIME_MODE: 'host', ENABLE_HOST_EXEC: 'true', PUBLIC_BASE_URL: '',
    HOST_DEFAULT_WORKDIR: root, MCP_JOBS_ROOT: path.join(root, 'jobs'), MCP_EXEC_SLOT_ROOT: path.join(root, 'slots'),
  } });
  let logs = '';
  for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { logs = (logs + bytes).slice(-4000); });
  const exited = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
  try {
    let ready = false;
    for (let n = 0; n < 150; n++) {
      try { if ((await fetch(`http://127.0.0.1:${port}/readyz`, { signal: AbortSignal.timeout(500) })).ok) { ready = true; break; } } catch {}
      if (child.exitCode !== null || child.signalCode !== null) throw new Error(logs);
      await sleep(100);
    }
    assert(ready, logs);
    const output = {};
    for (const [name, request] of cases) {
      const response = await fetch(url, { ...request,
        headers: Object.fromEntries(Object.entries(request.headers).filter(([, value]) => value !== undefined)),
        body: request.body ? JSON.stringify(request.body) : undefined, signal: AbortSignal.timeout(5000) });
      const raw = await response.text();
      let body = raw;
      try { body = JSON.parse(raw); } catch {
        const event = raw.split(/\r?\n/).find(line => line.startsWith('data:'));
        if (event) body = JSON.parse(event.slice(5));
        else if (response.headers.get('content-type')?.startsWith('text/event-stream')) body = '<SSE probe>';
      }
      output[name] = { status: response.status, body: normalized(body) };
    }
    return output;
  } finally {
    if (child.exitCode === null && child.signalCode === null) { child.kill('SIGKILL'); await Promise.race([exited, sleep(5000)]); }
    assert(child.exitCode !== null || child.signalCode !== null, 'owned wire fixture stopped');
    await rm(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}
const rust = await probe(process.env.DEVBOX_RUST_REFERENCE_BINARY);
const cpp = await probe(process.env.DEVBOX_MCP_TEST_BINARY);
const differences = Object.keys(rust).filter(name => { try { assert.deepEqual(cpp[name], rust[name]); return false; } catch { return true; } });
console.log(JSON.stringify({ ok: differences.length === 0, total: cases.length, differences, rust, cpp }, null, 2));
if (differences.length) process.exitCode = 1;
