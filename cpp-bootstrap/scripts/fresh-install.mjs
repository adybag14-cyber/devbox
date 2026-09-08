import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { copyFile, mkdtemp, readFile, rm, mkdir } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-fresh-install-'));
const installedRepo = path.join(root, 'source with spaces');
const bundle = path.join(root, 'bundle');
const suffix = process.platform === 'win32' ? '.exe' : '';
const inputs = { 'devbox-setup': process.env.DEVBOX_SETUP_TEST_BINARY,
  'devbox-mcp': process.env.DEVBOX_MCP_TEST_BINARY, 'devbox-tui': process.env.DEVBOX_TUI_TEST_BINARY };
const env = { ...process.env, DEVBOX_PROJECT_ROOT: installedRepo, PUBLIC_BASE_URL: '', MCP_AUTH_MODE: 'none' };
const sha = file => readFile(file).then(bytes => createHash('sha256').update(bytes).digest('hex'));
const outputs = {};
async function run(file, args, cwd = installedRepo) {
  return runCheckedProcess(file, args, { cwd, env, timeoutMs: 30000, label: 'Packaged C++ installer fixture' });
}
try {
  await run('git', ['clone', '--quiet', '--no-hardlinks', repo, installedRepo], root);
  await mkdir(bundle);
  for (const [name, file] of Object.entries(inputs)) {
    assert(file && path.isAbsolute(file), `Supply ${name} test artifact as an absolute path`);
    await copyFile(file, path.join(bundle, name + suffix));
  }
  const setup = path.join(bundle, 'devbox-setup' + suffix);
  const tui = path.join(bundle, 'devbox-tui' + suffix);
  outputs.version = (await run(setup, ['--version'])).stdout.trim();
  assert.match(outputs.version, /C\+\+/);
  outputs.configure = (await run(setup, ['--repo', installedRepo, '--runtime', 'host', '--auth', 'none',
    '--host', '127.0.0.1', '--port', '18193', '--workspace', path.join(root, 'work with spaces'),
    '--skip-system-packages', '--skip-install', '--no-link', '--no-start'])).stdout;
  const config = await readFile(path.join(installedRepo, '.env'), 'utf8');
  assert.match(config, /^DEVBOX_MCP_IMPLEMENTATION=cpp$/m);
  assert.match(config, /^PORT=18193$/m);
  // This is the native installer itself selecting its bundled sibling. No
  // compiler, package manager, runtime startup or production path is involved.
  outputs.staging = (await run(setup, ['--repo', installedRepo, '--build-runtime-only'])).stdout;
  const staged = path.join(installedRepo, 'bin/native', 'devbox-mcp' + suffix);
  assert.equal(await sha(staged), await sha(inputs['devbox-mcp']));
  const info = JSON.parse((await run(staged, ['--build-info'])).stdout);
  assert.equal(info.implementation, 'cpp');
  assert.equal(info.binarySha256, await sha(staged));
  outputs.tui = (await run(tui, ['--diagnostics', '--no-color'])).stdout;
  assert.match(outputs.tui, /C\+\+ installer:/);
  assert(outputs.tui.includes(setup));
  console.log(JSON.stringify({ ok: true, binarySha256: info.binarySha256,
    source: info.gitSha, checks: ['fresh configuration', 'bundled runtime discovery',
      'verified staging', 'native build identity', 'TUI installer discovery'], outputs }, null, 2));
} finally {
  await rm(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}
