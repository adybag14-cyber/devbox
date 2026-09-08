import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert(['linux', 'darwin'].includes(process.platform), 'POSIX download fixture requires Linux or macOS');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-download-install-'));
const hashes = bytes => createHash('sha256').update(bytes).digest('hex');
const binaries = new Map();
for (const [name, variable] of [['devbox-setup', 'DEVBOX_SETUP_TEST_BINARY'],
  ['devbox-mcp', 'DEVBOX_MCP_TEST_BINARY'], ['devbox-tui', 'DEVBOX_TUI_TEST_BINARY']]) {
  assert(process.env[variable] && path.isAbsolute(process.env[variable]), `Supply ${variable}`);
  binaries.set(name, await readFile(process.env[variable]));
}
let assets, corrupt = false;
const server = createServer((request, response) => {
  const name = decodeURIComponent(new URL(request.url, 'http://localhost').pathname.slice(1));
  if (name === 'SHA256SUMS') {
    response.end([...assets].map(([asset, bytes]) => `${hashes(bytes)}  ${asset}\n`).join('')); return;
  }
  const bytes = assets.get(name);
  if (!bytes) { response.writeHead(404).end(); return; }
  response.end(corrupt && name.startsWith('devbox-mcp-') ? Buffer.from('intentionally corrupt fixture') : bytes);
});
server.listen(0, '127.0.0.1');
await new Promise((resolve, reject) => { server.once('listening', resolve); server.once('error', reject); });
const base = `http://127.0.0.1:${server.address().port}`;
const results = [];
try {
  const arch = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
  const platform = process.platform === 'darwin' ? 'macos' : 'linux';
  for (const mode of ['posix', 'termux-download-fixture']) {
    const install = path.join(root, mode, 'bin'); await mkdir(install, { recursive: true });
    const suffix = mode === 'posix' ? `${platform}-${arch}` : `android-${arch === 'aarch64' ? 'arm64-v8a' : arch}`;
    assets = new Map([...binaries].map(([name, bytes]) => [`${name}-${suffix}`, bytes]));
    const env = { ...process.env, DEVBOX_SETUP_RELEASE_BASE: base, DEVBOX_SETUP_INSTALL_DIR: install,
      DEVBOX_SETUP_INSTALL_PATH: path.join(install, 'devbox-setup'),
      DEVBOX_TUI_INSTALL_PATH: path.join(install, 'devbox-tui'), DEVBOX_MCP_INSTALL_PATH: path.join(install, 'devbox-mcp') };
    if (mode !== 'posix') {
      // Only downloader/package-command wiring is simulated here. Android
      // executable and real Termux runtime checks use their own native gate.
      const prefix = path.join(root, 'termux-prefix'), bin = path.join(prefix, 'bin');
      await mkdir(bin, { recursive: true }); await mkdir(path.join(prefix, 'tmp'));
      for (const [name, script] of Object.entries({ pkg: '#!/bin/sh\nexit 0\n',
        getprop: '#!/bin/sh\nprintf "21\\n"\n' })) {
        await writeFile(path.join(bin, name), script); await chmod(path.join(bin, name), 0o755);
      }
      env.PREFIX = prefix; env.PATH = `${bin}:${env.PATH}`;
    }
    const script = path.join(repo, 'scripts', mode === 'posix' ? 'install-devbox.sh' : 'install-termux.sh');
    const run = () => runCheckedProcess('sh', [script, '--version'], { cwd: root, env,
      timeoutMs: 60000, label: 'Local HTTP installer artifact fixture' });
    corrupt = true;
    for (const name of binaries.keys()) await writeFile(path.join(install, name), `previous ${name}`);
    await assert.rejects(run(), /Checksum verification failed/);
    for (const name of binaries.keys()) assert.equal(await readFile(path.join(install, name), 'utf8'), `previous ${name}`);
    corrupt = false;
    assert.match((await run()).stdout, /C\+\+/);
    for (const [name, bytes] of binaries) assert.equal(hashes(await readFile(path.join(install, name))), hashes(bytes));
    results.push({ mode, checks: ['three verified artifacts', 'native setup execution', 'corrupt runtime preserves all previous binaries'] });
  }
  console.log(JSON.stringify({ ok: true, checks: results }, null, 2));
} finally {
  server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
  await rm(root, { recursive: true, force: true });
}
