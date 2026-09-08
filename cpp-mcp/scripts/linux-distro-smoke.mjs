import assert from 'node:assert/strict';
import { createHash, randomUUID } from 'node:crypto';
import { copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert.equal(process.platform, 'linux', 'Run distro certification on an isolated Linux host');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const distributions = { ubuntu: ['ubuntu:26.04', 'apt'], debian: ['debian:13', 'apt'],
  fedora: ['fedora:44', 'dnf'], arch: ['archlinux:base', 'pacman'], alpine: ['alpine:3.23', 'apk'] };
const distribution = process.argv[2];
assert(distributions[distribution], 'Choose ubuntu, debian, fedora, arch or alpine');
const [image, family] = distributions[distribution];
const artifact = path.resolve(process.argv[3] || '.cpp-build/distro-artifact/package/bin');
const identity = `devbox-cpp-${distribution}-${randomUUID()}`;
const label = 'io.devbox.cpp.fixture';
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-distro-'));
const input = path.join(root, 'input'); await mkdir(path.join(input, 'bin'), { recursive: true });
async function run(file, args, options = {}) {
  return runCheckedProcess(file, args, { cwd: repo, timeoutMs: 120000,
    label: 'Native Linux distro certification', ...options });
}
try {
  const files = ['source.bundle', 'run.sh'];
  if (distribution !== 'alpine') for (const name of ['devbox-mcp', 'devbox-setup', 'devbox-tui']) {
    const file = `bin/${name}`; await copyFile(path.join(artifact, name), path.join(input, file)); files.push(file);
  }
  await copyFile(path.join(repo, 'cpp-mcp/scripts/linux-container-inner.sh'), path.join(input, 'run.sh'));
  await run('git', ['bundle', 'create', path.join(input, 'source.bundle'), 'HEAD']);
  const source = (await run('git', ['rev-parse', 'HEAD'])).stdout.trim(); assert.match(source, /^[a-f0-9]{40}$/);
  await writeFile(path.join(input, 'SHA256SUMS'), (await Promise.all(files.map(async file =>
    `${createHash('sha256').update(await readFile(path.join(input, file))).digest('hex')}  ${file}\n`))).join(''));
  await run('docker', ['pull', image], { timeoutMs: 10 * 60 * 1000, stdio: 'inherit' });
  const imageInfo = JSON.parse((await run('docker', ['image', 'inspect', image])).stdout)[0];
  console.log(JSON.stringify({ distribution, image, digest: imageInfo.RepoDigests, imageId: imageInfo.Id, source }));
  const args = ['run', '--rm', '--name', identity, '--label', `${label}=${identity}`,
    '-e', `DEVBOX_EXPECTED_SOURCE=${source}`, '-e', `DEVBOX_DISTRO_FAMILY=${family}`, '-v', `${input}:/input:ro`];
  if (distribution === 'alpine') {
    const cache = path.join(repo, '.cpp-build/alpine-cache'); await mkdir(cache, { recursive: true });
    args.push('-v', `${cache}:/cache`);
  }
  args.push(imageInfo.Id, 'sh', '/input/run.sh');
  await run('docker', args, { timeoutMs: (distribution === 'alpine' ? 75 : 30) * 60 * 1000, stdio: 'inherit' });
  console.log(JSON.stringify({ ok: true, distribution, imageId: imageInfo.Id, source }));
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
