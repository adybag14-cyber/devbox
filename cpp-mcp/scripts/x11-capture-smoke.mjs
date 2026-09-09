import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert.equal(process.platform, 'linux');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const binary = process.env.DEVBOX_MCP_TEST_BINARY; assert(binary && path.isAbsolute(binary));
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-x11-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const children = [];
function child(file, args, env, extraPipe = false) {
  const handle = spawn(file, args, { env, cwd: root, detached: true,
    stdio: extraPipe ? ['ignore', 'pipe', 'pipe', 'pipe'] : ['ignore', 'pipe', 'pipe'] });
  let output = ''; for (const stream of [handle.stdout, handle.stderr]) stream.on('data', bytes => { output = (output + bytes).slice(-16000); });
  const exited = new Promise((resolve, reject) => { handle.once('exit', resolve); handle.once('error', reject); });
  const state = { handle, exited, output: () => output }; children.push(state); return state;
}
const run = (file, args, env = process.env) => runCheckedProcess(file, args, {
  cwd: root, env, timeoutMs: 30000, label: 'Owned X11 capture fixture' });
try {
  await run('c++', ['-std=c++17', '-O2', path.join(repo, 'cpp-mcp/tests/capture_x11_fixture.cpp'), '-lX11', '-o', path.join(root, 'fixture')]);
  const display = child('Xvfb', ['-displayfd', '3', '-screen', '0', '800x600x24', '-nolisten', 'tcp', '-noreset'], process.env, true);
  const number = await new Promise((resolve, reject) => {
    let buffer = ''; const timer = setTimeout(() => reject(new Error(`Xvfb startup: ${display.output()}`)), 10000);
    display.handle.stdio[3].on('data', bytes => { buffer += bytes; if (buffer.includes('\n')) { clearTimeout(timer); resolve(buffer.trim()); } });
    display.exited.then(() => { clearTimeout(timer); reject(new Error(`Xvfb exited: ${display.output()}`)); }, reject);
  });
  assert.match(number, /^\d+$/);
  const env = { ...process.env, DISPLAY: `:${number}`, XDG_SESSION_TYPE: 'x11' }; delete env.WAYLAND_DISPLAY;
  const window = child(path.join(root, 'fixture'), [], env);
  const deadline = Date.now() + 10000;
  while (!window.output().includes('ready ')) {
    assert.equal(window.handle.exitCode, null, window.output()); assert(Date.now() < deadline); await delay(50);
  }
  const checks = [];
  for (const mode of ['program', 'display']) {
    const output = path.join(root, `${mode}.png`);
    const args = ['--capture-worker', output, mode, '82'];
    if (mode === 'program') args.push(String(window.handle.pid), 'false');
    const metadata = JSON.parse((await run(binary, args, env)).stdout);
    const bytes = await readFile(output);
    assert(bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10])));
    const expected = mode === 'program' ? '360 240' : '800 600';
    assert.equal((await run('identify', ['-format', '%w %h', output])).stdout.trim(), expected);
    if (mode === 'program') {
      assert.equal(metadata.window_owner_pid, window.handle.pid);
      assert.equal(metadata.width, 360); assert.equal(metadata.height, 240);
      assert.equal(metadata.process_tree_fallback, false);
      const left = (await run('convert', [output, '-crop', '1x1+40+40', '-depth', '8', 'txt:-'])).stdout;
      const right = (await run('convert', [output, '-crop', '1x1+300+40', '-depth', '8', 'txt:-'])).stdout;
      assert.match(left, /#E6642D/i); assert.match(right, /#2878E6/i);
    }
    checks.push({ mode, dimensions: expected, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex'), metadata });
  }
  await writeFile(path.join(repo, '.cpp-build/x11-capture-result.json'), JSON.stringify({ ok: true, checks }, null, 2));
  console.log(JSON.stringify({ ok: true, checks }, null, 2));
} finally {
  for (const state of children.reverse()) {
    if (state.handle.exitCode === null && state.handle.signalCode === null) {
      state.handle.kill(); await Promise.race([state.exited, delay(3000)]);
      if (state.handle.exitCode === null && state.handle.signalCode === null) { state.handle.kill('SIGKILL'); await state.exited; }
    }
  }
  await rm(root, { recursive: true, force: true });
}
