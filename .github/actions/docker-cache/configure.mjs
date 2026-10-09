import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdir, readFile, lstat, writeFile, rename, rm } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as pause } from 'node:timers/promises';

const mirror = 'https://mirror.gcr.io';

export function withPublicMirror(config) {
  assert(config && typeof config === 'object' && !Array.isArray(config), 'Docker config must be an object');
  const previous = Object.hasOwn(config, 'registry-mirrors') ? config['registry-mirrors'] : [];
  assert(Array.isArray(previous) && previous.every(value => typeof value === 'string'),
    'Existing registry mirrors must be a string array');
  return { ...config, 'registry-mirrors': [mirror,
    ...previous.filter(value => value.replace(/\/$/, '') !== mirror)] };
}

async function configure() {
  // Never edit a developer machine or a shared self-hosted runner's daemon.
  assert.equal(process.platform, 'linux');
  assert.equal(process.env.GITHUB_ACTIONS, 'true');
  assert.equal(process.env.RUNNER_ENVIRONMENT, 'github-hosted');
  assert.equal(process.env.RUNNER_OS, 'Linux');
  const running = execFileSync('docker', ['ps', '--quiet'], { timeout: 10000, encoding: 'utf8' }).trim();
  assert.equal(running, '', 'Cache activation requires an empty dedicated CI Docker daemon');
  const file = '/etc/docker/daemon.json';
  await mkdir(path.dirname(file), { recursive: true });
  let prior = {}, mode = 0o644;
  try {
    const info = await lstat(file);
    assert(info.isFile() && !info.isSymbolicLink(), 'Docker config must be a regular file');
    mode = info.mode & 0o777;
    prior = JSON.parse(await readFile(file, 'utf8'));
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  const temporary = `${file}.devbox-${process.pid}.tmp`;
  let created = false;
  try {
    await writeFile(temporary, JSON.stringify(withPublicMirror(prior), null, 2) + '\n', { flag: 'wx', mode });
    created = true;
    execFileSync('dockerd', ['--validate', '--config-file', temporary], { timeout: 10000, stdio: 'inherit' });
    await rename(temporary, file);
  } finally {
    if (created) await rm(temporary, { force: true });
  }
  // The hosted runner acknowledged a reload while its pull resolver continued
  // to hit Hub. Activate the setting from startup on this verified empty,
  // dedicated runner; never restart a developer or self-hosted daemon.
  execFileSync('systemctl', ['restart', 'docker'], { timeout: 30000, stdio: 'inherit' });
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const mirrors = JSON.parse(execFileSync('docker', ['info', '--format', '{{json .RegistryConfig.Mirrors}}'],
      { timeout: 3000, encoding: 'utf8' }));
    if (Array.isArray(mirrors) && mirrors.some(value => value.replace(/\/$/, '') === mirror)) {
      const version = execFileSync('docker', ['version', '--format', '{{.Server.Version}}'],
        { timeout: 5000, encoding: 'utf8' }).trim();
      console.log(JSON.stringify({ configured: true, mirror, daemonRestarted: true, version,
        imageReferencesChanged: false, credentialsRequired: false }));
      return;
    }
    await pause(250);
  }
  throw new Error('Docker did not acknowledge the registry cache configuration');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await configure();
