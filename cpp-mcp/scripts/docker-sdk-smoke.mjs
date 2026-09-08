// Runs only against uniquely named containers and volumes created by this test.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert.equal(process.platform, 'linux', 'Run live Docker certification on an isolated Linux host');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const binary = process.env.DEVBOX_MCP_TEST_BINARY;
assert(binary && path.isAbsolute(binary), 'Supply the candidate DEVBOX_MCP_TEST_BINARY');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const binaryHash = sha(await readFile(binary));
const identity = `devbox-cpp-${randomUUID()}`;
const imageName = `${identity}:fixture`, containerName = identity, volumeName = `${identity}-tmp`;
const label = 'io.devbox.cpp.fixture';
const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-docker-'));
const workspace = path.join(root, 'workspace'), hostBin = path.join(root, 'host-bin');
await mkdir(workspace); await mkdir(hostBin);
const env = { ...process.env, GH_CONFIG_DIR: path.join(root, 'gh'),
  GIT_CONFIG_GLOBAL: path.join(root, 'gitconfig'), GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_COUNT: '0' };
for (const key of ['GH_TOKEN', 'GITHUB_TOKEN', 'GH_ENTERPRISE_TOKEN', 'GITHUB_ENTERPRISE_TOKEN']) delete env[key];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const outputs = [];
let child, exited, client, imageId;
async function run(file, args, options = {}) {
  return runCheckedProcess(file, args, { cwd: root, env, timeoutMs: 120000,
    label: 'Owned Docker integration fixture', ...options });
}
const docker = (...args) => run('docker', args);
const inspect = async name => JSON.parse((await docker('inspect', '--type', 'container', name)).stdout)[0];
async function eventually(check, description, timeout = 10000) {
  const deadline = Date.now() + timeout;
  for (;;) {
    if (await check()) return;
    assert(Date.now() < deadline, description);
    await delay(100);
  }
}
async function invoke(name, args = {}, success = true) {
  const result = await client.callTool({ name, arguments: args });
  assert.equal(result.isError, !success, `${name}: ${JSON.stringify(result)}`);
  return result.structuredContent;
}
// Host and container GitHub operations use this explicit, non-secret fixture;
// the actual gh binary is checked before the container fixture is installed.
const fixtureToken = 'devbox-cpp-integration-non-secret-token';
const ghFixture = `#!/bin/sh
set -eu
case "$1 $2" in
  'auth status') printf 'Logged in to fixture github.com account\\n' ;;
  'auth token') printf '${fixtureToken}\\n' ;;
  'auth login')
    IFS= read -r token
    if [ -f /tmp/devbox-gh-fail ]; then printf '%s\\n' "$token" >&2; exit 1; fi
    printf '%s' "$token" > /tmp/devbox-gh-received ;;
  'auth setup-git') : ;;
  *) echo 'Unexpected GitHub fixture command' >&2; exit 2 ;;
esac
`;
try {
  await docker('version', '--format', '{{.Server.Version}}');
  await copyFile(path.join(repo, 'runtime.Dockerfile'), path.join(root, 'Dockerfile'));
  await run('docker', ['build', '--label', `${label}=${identity}`, '-t', imageName, root],
    { timeoutMs: 20 * 60 * 1000, stdio: 'inherit' });
  const image = JSON.parse((await docker('image', 'inspect', imageName)).stdout)[0];
  imageId = image.Id;
  assert.equal(image.Config.Labels[label], identity);
  await docker('volume', 'create', '--label', `${label}=${identity}`, volumeName);
  // A legacy container has a writable-layer /tmp that recreation must preserve.
  await docker('run', '-d', '--name', containerName, '--label', `${label}=${identity}`,
    '--init', '-w', '/workspace', '-v', `${workspace}:/workspace`, imageName, 'sleep', 'infinity');
  await docker('exec', containerName, 'sh', '-c',
    "printf 'legacy tmp bytes' > /tmp/legacy-marker; mkdir -p /tmp/nested; printf 'nested bytes' > /tmp/nested/marker");
  await writeFile(env.GIT_CONFIG_GLOBAL, '[user]\n\tname = Devbox Fixture\n\temail = fixture@example.invalid\n');
  await writeFile(path.join(hostBin, 'gh'), ghFixture); await chmod(path.join(hostBin, 'gh'), 0o755);
  const port = await new Promise((resolve, reject) => {
    const server = net.createServer(); server.once('error', reject);
    server.listen(0, '127.0.0.1', () => { const number = server.address().port; server.close(() => resolve(number)); });
  });
  const base = `http://127.0.0.1:${port}`;
  child = spawn(binary, [], { cwd: root, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...env, PATH: `${hostBin}:${env.PATH}`, DEVBOX_PROJECT_ROOT: root, HOST: '127.0.0.1', PORT: String(port),
      MCP_AUTH_MODE: 'none', PUBLIC_BASE_URL: '', DEVBOX_RUNTIME_MODE: 'docker', ENABLE_HOST_EXEC: 'true',
      DEVBOX_AUTO_START: 'true', DEVBOX_CONTAINER_NAME: containerName, DEVBOX_IMAGE_NAME: imageName,
      DEVBOX_TMP_VOLUME_NAME: volumeName, DEVBOX_RETIRED_CONTAINER_GRACE_MS: '250',
      HOST_WORKSPACE_PATH: workspace, HOST_DEFAULT_WORKDIR: workspace, HOST_SHELL: '/bin/sh',
      DEVBOX_WORKSPACE_PATH: '/workspace', DEVBOX_DEFAULT_USER: '', DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE: '0',
      MCP_JOBS_ROOT: path.join(root, 'jobs'), MCP_EXEC_SLOT_ROOT: path.join(root, 'slots'),
      MAX_COMMAND_OUTPUT_CHARS: '65536', MAX_MCP_TRANSFER_CHARS: '4000000' } });
  let serverOutput = '';
  for (const stream of [child.stdout, child.stderr]) stream.on('data', value => { serverOutput = (serverOutput + value).slice(-16000); });
  exited = new Promise((resolve, reject) => { child.once('exit', resolve); child.once('error', reject); });
  await eventually(async () => {
    assert.equal(child.exitCode, null, serverOutput);
    try {
      const response = await fetch(base, { signal: AbortSignal.timeout(1000) });
      if (!response.ok) return false;
      assert.equal((await response.json()).build.binarySha256, binaryHash); return true;
    } catch (error) { if (error.code === 'ERR_ASSERTION') throw error; return false; }
  }, 'Owned C++ Docker-mode server did not become ready', 20000);
  client = new Client({ name: 'cpp-live-docker-certification', version: '1' });
  await client.connect(new StreamableHTTPClientTransport(new URL(`${base}/mcp`)));
  assert.equal((await client.listTools()).tools.length, 45);
  assert.equal((await invoke('devbox_start')).data.running, true);
  const program = await invoke('devbox_run_program', { program: 'node', args: ['-e',
    'process.stdout.write(JSON.stringify({platform:process.platform,cwd:process.cwd(),args:process.argv.slice(1)}))',
    'spaces and "quotes"', '$(literal)'] });
  assert.deepEqual(JSON.parse(program.stdout), { platform: 'linux', cwd: '/workspace', args: ['spaces and "quotes"', '$(literal)'] });
  assert.equal((await invoke('devbox_exec', { command: "printf 'shell fixture'" })).stdout, 'shell fixture');
  outputs.push('Docker argv, shell, cwd and 45-tool registration');

  await invoke('devbox_write_file', { path: 'nested/utf8.txt', content: 'native αβγ\nneedle 123\n' });
  await invoke('devbox_write_file', { path: '/workspace/nested/utf8.txt', content: 'appended\n', append: true });
  assert.equal((await invoke('devbox_read_file', { path: 'nested/utf8.txt' })).stdout,
    'native αβγ\nneedle 123\nappended\n');
  assert.match((await invoke('devbox_list_files', { path: '.', recursive: true })).stdout, /utf8\.txt/);
  assert.match((await invoke('devbox_search_files', { pattern: 'needle', path: '/workspace' })).stdout, /needle 123/);
  const bytes = Buffer.from(Array.from({ length: 300000 }, (_, index) => index % 256));
  const written = await invoke('devbox_write_large_file', { path: 'nested/payload.bin',
    content_base64: bytes.toString('base64'), expected_sha256: sha(bytes) });
  assert.equal(written.data.verified, true);
  const read = await invoke('devbox_read_large_file', { path: 'nested/payload.bin', offset_bytes: 101, max_bytes: 280000 });
  assert.deepEqual(Buffer.from(read.data.content_base64, 'base64'), bytes.subarray(101, 280101));
  assert.deepEqual(await readFile(path.join(workspace, 'nested/payload.bin')), bytes);
  await invoke('devbox_write_large_file', { path: 'nested/payload.bin', content: 'bad', expected_sha256: '0'.repeat(64) }, false);
  assert.deepEqual(await readFile(path.join(workspace, 'nested/payload.bin')), bytes, 'hash mismatch preserves previous bytes');
  outputs.push('Container text, binary, offset read, search and hash-failure preservation');

  const started = (await invoke('devbox_run_program_start', { program: 'node', args: ['-e',
    "require('fs').writeFileSync('/workspace/job-done','done');console.log('docker detached job')"] })).data;
  assert.equal((await invoke('devbox_job_status', { job_id: started.id, wait_seconds: 15 })).data.status, 'succeeded');
  assert.match((await invoke('devbox_job_logs', { job_id: started.id })).data.stdout, /docker detached job/);
  assert.equal(await readFile(path.join(workspace, 'job-done'), 'utf8'), 'done');
  outputs.push('Detached container process and persisted terminal logs');

  assert.match((await invoke('devbox_run_program', { program: 'gh', args: ['--version'] })).stdout, /gh version/);
  await invoke('devbox_github_auth_status', {}, false);
  await docker('cp', path.join(hostBin, 'gh'), `${containerName}:/usr/local/bin/gh`);
  const synced = (await invoke('devbox_sync_github_auth_from_host')).data;
  assert.equal(synced.userName, 'Devbox Fixture'); assert.equal(synced.userEmail, 'fixture@example.invalid');
  assert.equal((await docker('exec', containerName, 'cat', '/tmp/devbox-gh-received')).stdout, fixtureToken);
  await docker('exec', containerName, 'touch', '/tmp/devbox-gh-fail');
  const failed = await invoke('devbox_sync_github_auth_from_host', {}, false);
  assert(!JSON.stringify(failed).includes(fixtureToken)); assert.match(JSON.stringify(failed), /redacted/);
  outputs.push('Real gh discovery plus isolated auth-sync identity, stdin and redaction');

  const original = (await inspect(containerName)).Id;
  assert.equal((await invoke('devbox_stop')).data.running, false);
  assert.equal((await invoke('devbox_start')).data.running, true);
  assert.equal((await invoke('devbox_restart')).data.running, true);
  assert.equal((await inspect(containerName)).Id, original);
  const recreated = (await invoke('devbox_recreate')).data;
  assert.notEqual(recreated.id, original);
  assert(recreated.mounts.some(mount => mount.Name === volumeName && mount.Destination === '/tmp'));
  assert.equal((await docker('exec', containerName, 'cat', '/tmp/legacy-marker')).stdout, 'legacy tmp bytes');
  assert.equal((await docker('exec', containerName, 'cat', '/tmp/nested/marker')).stdout, 'nested bytes');
  await eventually(async () => !(await docker('ps', '-aq', '--filter', `name=^/${containerName}-retired-`)).stdout.trim(),
    'Retired owned container was not cleaned after its grace period');
  await invoke('devbox_recreate');
  assert.equal((await docker('exec', containerName, 'cat', '/tmp/legacy-marker')).stdout, 'legacy tmp bytes');
  assert.deepEqual(await readFile(path.join(workspace, 'nested/payload.bin')), bytes);
  assert.equal(await (await fetch(`${base}/healthz`)).text(), 'ok');
  outputs.push('Stop/start/restart, legacy /tmp migration, grace cleanup and volume-preserving recreation');
  console.log(JSON.stringify({ ok: true, binarySha256: binaryHash, imageId, checks: outputs }, null, 2));
} finally {
  await client?.close().catch(() => {});
  if (child && child.exitCode === null && child.signalCode === null) {
    child.kill(); await Promise.race([exited, delay(10000)]);
    if (child.exitCode === null && child.signalCode === null) { child.kill('SIGKILL'); await exited; }
  }
  if (imageId) {
    const names = (await docker('ps', '-a', '--format', '{{.Names}}', '--filter', `name=^/${containerName}`)).stdout.trim().split('\n').filter(Boolean);
    for (const name of names) {
      assert(name === containerName || name.startsWith(`${containerName}-retired-`));
      const info = await inspect(name);
      assert.equal(info.Image, imageId, 'cleanup requires the fixture image identity');
      assert(info.Mounts.some(mount => mount.Source === workspace && mount.Destination === '/workspace'));
      // Container file APIs intentionally run as root in this fixture. Restore
      // the test runner's ownership only after verifying this exact bind mount.
      if (!info.State.Running) await docker('start', info.Id);
      await docker('exec', '--user', '0', info.Id, 'chown', '-R', '-h',
        `${process.getuid()}:${process.getgid()}`, '/workspace');
      await docker('rm', '-f', info.Id);
    }
    const volumes = (await docker('volume', 'ls', '--format', '{{.Name}}', '--filter', `label=${label}=${identity}`)).stdout.trim().split('\n').filter(Boolean);
    for (const name of volumes) {
      assert.equal(name, volumeName);
      const volume = JSON.parse((await docker('volume', 'inspect', name)).stdout)[0];
      assert.equal(volume.Labels[label], identity); await docker('volume', 'rm', name);
    }
    const image = JSON.parse((await docker('image', 'inspect', imageId)).stdout)[0];
    assert.equal(image.Config.Labels[label], identity); await docker('image', 'rm', imageName);
  }
  await rm(root, { recursive: true, force: true });
}
