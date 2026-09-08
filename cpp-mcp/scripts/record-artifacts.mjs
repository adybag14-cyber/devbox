import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const options = Object.fromEntries(process.argv.slice(2).map((arg, i, args) =>
  arg.startsWith('--') ? [arg.slice(2), args[i + 1]] : null).filter(Boolean));
const android = options.android;
assert(!android || ['arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'].includes(android));
const packageRoot = path.resolve(options.package || path.join(repo, '.cpp-build/package'));
const bin = options.bin ?? (android ? '' : 'bin');
const extension = !android && process.platform === 'win32' ? '.exe' : '';
const run = async (file, args) => (await runCheckedProcess(file, args,
  { cwd: repo, timeoutMs: 30000, label: 'C++ artifact provenance' })).stdout.trim();
const gitSha = await run('git', ['rev-parse', 'HEAD']);
const sourceTree = await run('git', ['rev-parse', 'HEAD^{tree}']);
assert.match(gitSha, /^[a-f0-9]{40}$/);
assert.match(sourceTree, /^[a-f0-9]{40}$/);
assert.equal(await run('git', ['status', '--porcelain', '--untracked-files=all']), '', 'Release inputs require clean source');
const setupVersion = (await readFile(path.join(repo, 'cpp-bootstrap/VERSION'), 'utf8')).trim();
const portStatus = JSON.parse(await readFile(path.join(repo, 'cpp-mcp/port-status.json'), 'utf8'));
const releaseReady = portStatus.complete === true && portStatus.production_cutover_allowed === true;
let build, target;
if (android) {
  const header = await readFile(path.join(repo, '.cpp-build/android', android, 'cpp-mcp/generated/build_identity.hpp'), 'utf8');
  const field = name => {
    const match = header.match(new RegExp(`const char\\* ${name} = ("[^"\\n]*");`));
    assert(match, `Missing generated Android identity field ${name}`);
    return JSON.parse(match[1]);
  };
  assert.equal(field('sha'), gitSha); assert.equal(field('tree'), sourceTree); assert.equal(field('dirty'), 'false');
  build = { implementation: 'cpp', gitSha, sourceTree, sourceDirty: false, sanitizers: false,
    sourceFingerprint: field('fingerprint'), compiler: field('compiler'), runtimeVersion: field('version') };
  target = `android-${android}`;
} else {
  const runtime = path.join(packageRoot, bin, 'devbox-mcp' + extension);
  build = JSON.parse(await run(runtime, ['--build-info']));
  assert.equal(build.implementation, 'cpp'); assert.equal(build.gitSha, gitSha);
  assert.equal(build.sourceTree, sourceTree); assert.equal(build.sourceDirty, false);
  assert.equal(typeof build.sanitizers, 'boolean');
  const parity = JSON.parse(await run(runtime, ['--parity-report']));
  assert.equal(parity.complete === true && parity.cutover_allowed === true, releaseReady);
  assert.equal(await run(path.join(packageRoot, bin, 'devbox-setup' + extension), ['--version']), `devbox-setup ${setupVersion} (C++)`);
  assert.match(await run(path.join(packageRoot, bin, 'devbox-tui' + extension), ['--version']), new RegExp(setupVersion.replaceAll('.', '\\.')));
  const arch = process.arch === 'arm64' ? 'aarch64' : process.arch === 'x64' ? 'x86_64' : '';
  assert(arch, 'Unsupported native release architecture');
  const os = process.platform === 'win32' ? 'windows' : process.platform === 'darwin' ? 'macos'
    : process.report.getReport().header.glibcVersionRuntime ? 'linux' : 'linux-musl';
  target = `${os}-${arch}`;
}
const binaries = [];
for (const name of ['devbox-mcp', 'devbox-setup', 'devbox-tui']) {
  const file = path.posix.join(bin, name + extension);
  const bytes = await readFile(path.join(packageRoot, file));
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  assert(bytes.length > 10000, `Invalid native binary ${file}`);
  if (name === 'devbox-mcp' && !android) assert.equal(build.binarySha256, sha256);
  binaries.push({ name, file, bytes: bytes.length, sha256 });
}
const manifest = { schema: 1, implementation: 'cpp', target, gitSha, sourceTree, setupVersion, releaseReady, build,
  dependencyBaseline: JSON.parse(await readFile(path.join(repo, 'vcpkg.json'), 'utf8'))['builtin-baseline'],
  ...(android ? { ndk: '29.0.14206865', minimumAndroidApi: 21 } : {}), binaries };
await writeFile(path.join(packageRoot, 'build-manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(JSON.stringify(manifest));
