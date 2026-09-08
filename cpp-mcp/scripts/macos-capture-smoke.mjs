import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert.equal(process.platform, 'darwin');
assert(process.env.GITHUB_ACTIONS === 'true' || process.env.DEVBOX_CPP_CAPTURE_OWNED_UI === '1',
  'Use a disposable CI desktop or explicitly select the owned-window fixture');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const binary = process.env.DEVBOX_MCP_TEST_BINARY; assert(binary && path.isAbsolute(binary));
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-cocoa-'));
const fixture = path.join(root, 'fixture');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const run = (file, args) => runCheckedProcess(file, args, { cwd: root, timeoutMs: 30000,
  label: 'Owned macOS capture fixture' });
let child, exited, output = '';
try {
  await run('/usr/bin/clang++', ['-std=c++17', '-fobjc-arc',
    path.join(repo, 'cpp-mcp/tests/capture_macos_fixture.mm'), '-framework', 'Cocoa', '-o', fixture]);
  child = spawn(fixture, [], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
  for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => { output = (output + bytes).slice(-16000); });
  exited = new Promise((resolve, reject) => { child.once('exit', resolve); child.once('error', reject); });
  const deadline = Date.now() + 10000;
  while (!output.includes('ready ')) {
    assert.equal(child.exitCode, null, output); assert(Date.now() < deadline, output); await delay(50);
  }
  await delay(250);
  const checks = [];
  for (const mode of ['program', 'display']) {
    const file = path.join(root, `${mode}.png`);
    const args = ['--capture-worker', file, mode, '82'];
    if (mode === 'program') args.push(String(child.pid), 'false');
    const metadata = JSON.parse((await run(binary, args)).stdout);
    const bytes = await readFile(file);
    assert(bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10])));
    const dimensions = (await run('/usr/bin/sips', ['-g', 'pixelWidth', '-g', 'pixelHeight', file])).stdout;
    assert.match(dimensions, /pixelWidth: [1-9]\d+/); assert.match(dimensions, /pixelHeight: [1-9]\d+/);
    let decoded;
    if (mode === 'program') {
      assert.equal(metadata.window_owner_pid, child.pid);
      assert.equal(metadata.width, 360); assert.equal(metadata.height, 240);
      assert.equal(metadata.process_tree_fallback, false);
      decoded = JSON.parse((await run(fixture, ['--check-image', file])).stdout);
      assert.equal(decoded.color_pattern, true);
      assert.equal(decoded.width / metadata.width, decoded.height / metadata.height);
    }
    checks.push({ mode, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex'), metadata, decoded });
  }
  await writeFile(path.join(repo, '.cpp-build/macos-capture-result.json'), JSON.stringify({ ok: true, checks }, null, 2));
  console.log(JSON.stringify({ ok: true, checks }, null, 2));
} finally {
  if (child && child.exitCode === null && child.signalCode === null) {
    child.kill(); await Promise.race([exited, delay(3000)]);
    if (child.exitCode === null && child.signalCode === null) { child.kill('SIGKILL'); await exited; }
  }
  await rm(root, { recursive: true, force: true });
}
