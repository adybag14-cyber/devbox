import assert from 'node:assert/strict';
import { createHash, randomUUID } from 'node:crypto';
import { copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert.equal(process.platform, 'linux', 'Run the real Termux container gate on an isolated Linux host');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const artifact = path.resolve(process.argv[2] || '.cpp-build/termux-artifact');
// Resolved from the official x86_64 manifest, with no daemon or host changes.
const image = 'termux/termux-docker@sha256:81fe109e469dbea3773b426eb16399a311c047abe77c63842c38c744542ddd11';
const identity = `devbox-cpp-termux-${randomUUID()}`;
const label = 'io.devbox.cpp.fixture';
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-termux-'));
const input = path.join(root, 'input'); await mkdir(path.join(input, 'bin'), { recursive: true });
async function run(file, args, options = {}) {
  return runCheckedProcess(file, args, { cwd: repo, timeoutMs: 120000,
    label: 'Real Termux native certification', ...options });
}
try {
  for (const name of ['devbox-mcp', 'devbox-setup', 'devbox-tui']) await copyFile(path.join(artifact, name), path.join(input, 'bin', name));
  await copyFile(path.join(repo, 'cpp-mcp/scripts/termux-container-inner.sh'), path.join(input, 'run.sh'));
  await run('git', ['bundle', 'create', path.join(input, 'source.bundle'), 'HEAD']);
  const source = (await run('git', ['rev-parse', 'HEAD'])).stdout.trim();
  assert.match(source, /^[a-f0-9]{40}$/);
  const files = ['bin/devbox-mcp', 'bin/devbox-setup', 'bin/devbox-tui', 'source.bundle', 'run.sh'];
  const checksums = await Promise.all(files.map(async file =>
    `${createHash('sha256').update(await readFile(path.join(input, file))).digest('hex')}  ${file}\n`));
  await writeFile(path.join(input, 'SHA256SUMS'), checksums.join(''));
  await run('docker', ['pull', image], { timeoutMs: 10 * 60 * 1000, stdio: 'inherit' });
  await run('docker', ['run', '--rm', '--name', identity, '--label', `${label}=${identity}`,
    '-e', `DEVBOX_EXPECTED_SOURCE=${source}`, '-v', `${input}:/input:ro`, image, 'bash', '/input/run.sh'],
    { timeoutMs: 30 * 60 * 1000, stdio: 'inherit' });
  console.log(JSON.stringify({ ok: true, image, source, abi: 'x86_64', api: 21 }));
} finally {
  const names = (await run('docker', ['ps', '-a', '--filter', `label=${label}=${identity}`, '--format', '{{.Names}}'])).stdout.trim().split('\n').filter(Boolean);
  for (const name of names) {
    assert.equal(name, identity);
    const info = JSON.parse((await run('docker', ['inspect', name])).stdout)[0];
    assert.equal(info.Config.Labels[label], identity);
    assert(info.Mounts.some(mount => mount.Source === input && mount.Destination === '/input' && mount.RW === false));
    await run('docker', ['rm', '-f', info.Id]);
  }
  await rm(root, { recursive: true, force: true });
}
