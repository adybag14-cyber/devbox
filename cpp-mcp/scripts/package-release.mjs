import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { chmod, copyFile, mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

assert.equal(process.platform, 'linux', 'Assemble release artifacts on the isolated Linux packaging runner');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const input = path.resolve(process.argv[2] || '.cpp-build/release-input');
const output = path.resolve(process.argv[3] || '.cpp-build/release-assets');
const run = async (file, args) => (await runCheckedProcess(file, args,
  { cwd: repo, timeoutMs: 120000, label: 'Verified C++ release packaging' })).stdout.trim();
const expectedSha = await run('git', ['rev-parse', 'HEAD']);
const expectedTree = await run('git', ['rev-parse', 'HEAD^{tree}']);
const version = (await readFile(path.join(repo, 'cpp-bootstrap/VERSION'), 'utf8')).trim();
if (process.env.GITHUB_REF_TYPE === 'tag') assert.equal(process.env.GITHUB_REF_NAME, `bootstrap-v${version}`);
const baseline = JSON.parse(await readFile(path.join(repo, 'vcpkg.json'), 'utf8'))['builtin-baseline'];
const targets = {
  'windows-x86_64': 'cpp-native-windows-2025-0/package',
  'linux-x86_64': 'cpp-native-ubuntu-24.04-0/package',
  'linux-aarch64': 'cpp-native-ubuntu-24.04-arm-0/package',
  'macos-aarch64': 'cpp-native-macos-15-0/package',
  'macos-x86_64': 'cpp-native-macos-15-intel-0/package',
  'linux-musl-x86_64': 'cpp-musl-alpine-3.23',
  ...Object.fromEntries(['arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'].map(abi => [`android-${abi}`, `cpp-android-${abi}`])),
};
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const machines = { x86_64: 62, aarch64: 183, 'arm64-v8a': 183, 'armeabi-v7a': 40, x86: 3 };
function assertArchitecture(bytes, target) {
  if (target.startsWith('windows-')) {
    assert.equal(bytes.subarray(0, 2).toString(), 'MZ');
    const pe = bytes.readUInt32LE(0x3c);
    assert.equal(bytes.readUInt32LE(pe), 0x4550); assert.equal(bytes.readUInt16LE(pe + 4), 0x8664);
  } else if (target.startsWith('macos-')) {
    assert.equal(bytes.readUInt32LE(0), 0xfeedfacf);
    assert.equal(bytes.readUInt32LE(4), target.endsWith('aarch64') ? 0x100000c : 0x1000007);
  } else {
    assert.equal(bytes.subarray(0, 4).toString('hex'), '7f454c46'); assert.equal(bytes[5], 1);
    const architecture = Object.keys(machines).find(arch => target.endsWith(`-${arch}`));
    assert.equal(bytes.readUInt16LE(18), machines[architecture]);
  }
}
await mkdir(output, { recursive: true });
assert.deepEqual(await readdir(output), [], 'Refuse to mix release assets with an earlier packaging attempt');
const manifests = [];
const scratch = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-release-'));
try {
  for (const [target, directory] of Object.entries(targets)) {
    const source = path.join(input, directory);
    const manifest = JSON.parse(await readFile(path.join(source, 'build-manifest.json'), 'utf8'));
    assert.equal(manifest.schema, 1); assert.equal(manifest.target, target);
    assert.equal(manifest.implementation, 'cpp'); assert.equal(manifest.setupVersion, version);
    if (process.env.GITHUB_REF_TYPE === 'tag') assert.equal(manifest.releaseReady, true, 'Incomplete candidates cannot be published');
    assert.equal(manifest.gitSha, expectedSha); assert.equal(manifest.sourceTree, expectedTree);
    assert.equal(manifest.dependencyBaseline, baseline);
    assert.equal(manifest.build.gitSha, expectedSha); assert.equal(manifest.build.sourceTree, expectedTree);
    assert.equal(manifest.build.sourceDirty, false); assert.equal(manifest.build.sanitizers, false);
    assert.deepEqual(manifest.binaries.map(binary => binary.name).sort(), ['devbox-mcp', 'devbox-setup', 'devbox-tui']);
    const bundle = path.join(scratch, target); await mkdir(bundle);
    const extension = target.startsWith('windows-') ? '.exe' : '';
    for (const binary of manifest.binaries) {
      const expectedFile = `${target.startsWith('android-') ? '' : 'bin/'}${binary.name}${extension}`;
      assert.equal(binary.file, expectedFile);
      const bytes = await readFile(path.join(source, expectedFile));
      assert.equal(bytes.length, binary.bytes); assert.equal(digest(bytes), binary.sha256);
      assertArchitecture(bytes, target);
      const standalone = path.join(output, `${binary.name}-${target}${extension}`);
      await writeFile(standalone, bytes, { flag: 'wx', mode: 0o755 });
      await copyFile(standalone, path.join(bundle, binary.name + extension));
      await chmod(path.join(bundle, binary.name + extension), 0o755);
    }
    await writeFile(path.join(bundle, 'build-manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
    const archive = path.join(output, `devbox-${target}.${extension ? 'zip' : 'tar.gz'}`);
    const names = [...manifest.binaries.map(binary => binary.name + extension), 'build-manifest.json'];
    if (extension) await run('zip', ['-j', archive, ...names.map(name => path.join(bundle, name))]);
    else await run('tar', ['-czf', archive, '-C', bundle, ...names]);
    const extracted = path.join(scratch, `${target}-extracted`); await mkdir(extracted);
    if (extension) await run('unzip', ['-q', archive, '-d', extracted]);
    else await run('tar', ['-xzf', archive, '-C', extracted]);
    for (const name of names) assert.equal(digest(await readFile(path.join(extracted, name))), digest(await readFile(path.join(bundle, name))));
    manifests.push(manifest);
  }
  await writeFile(path.join(output, 'build-provenance.json'), `${JSON.stringify({ schema: 1, implementation: 'cpp',
    gitSha: expectedSha, sourceTree: expectedTree, version, dependencyBaseline: baseline, targets: manifests }, null, 2)}\n`);
  const names = (await readdir(output)).sort();
  await writeFile(path.join(output, 'SHA256SUMS'), (await Promise.all(names.map(async name =>
    `${digest(await readFile(path.join(output, name)))}  ${name}\n`))).join(''));
  assert.equal(names.length, 41, 'Ten targets require 30 binaries, ten archives and provenance');
  console.log(JSON.stringify({ ok: true, source: expectedSha, version, targets: Object.keys(targets), verifiedFiles: names.length, output }));
} finally {
  await rm(scratch, { recursive: true, force: true });
}
