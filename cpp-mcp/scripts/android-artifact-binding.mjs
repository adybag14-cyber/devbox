import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile, writeFile} from 'node:fs/promises';
import path from 'node:path';

const names = {
  'devbox-mcp': 'cpp-mcp/devbox-mcp',
  'devbox-setup': 'cpp-bootstrap/devbox-setup',
  'devbox-tui': 'setup-tui/devbox-tui',
};
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const abis = ['arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'];
const receiptName = 'android-build-binding.json';

async function identity(buildDirectory) {
  const bytes = await readFile(path.join(buildDirectory, 'cpp-mcp/generated/build_identity.hpp'));
  assert(bytes.length <= 65536, 'Bounded generated Android identity required');
  const header = bytes.toString('utf8');
  const field = name => {
    const matches = [...header.matchAll(new RegExp(String.raw`const char\* ${name} = ("(?:[^"\\\n]|\\.)*");`, 'g'))];
    assert.equal(matches.length, 1, `Unique generated Android identity field ${name} required`);
    return JSON.parse(matches[0][1]);
  };
  assert.match(field('sha'), /^[a-f0-9]{40}$/u);
  assert.match(field('tree'), /^[a-f0-9]{40}$/u);
  assert.match(field('fingerprint'), /^[a-f0-9]{64}$/u);
  assert(['true', 'false'].includes(field('dirty')));
  return {headerSha256: digest(bytes), build: {
    implementation: 'cpp', gitSha: field('sha'), sourceTree: field('tree'),
    sourceDirty: field('dirty') === 'true', sanitizers: false,
    sourceFingerprint: field('fingerprint'), compiler: field('compiler'),
    runtimeVersion: field('version'), nativeVersion: field('version'),
    packageVersion: field('package_version'), releaseTag: field('release_tag') || null,
  }};
}

// Called only after the checked CMake build invocation succeeds. This is a
// build-output binding, not an independently executed Android runtime identity.
export async function writeAndroidBuildBinding(buildDirectory, abi) {
  assert(abis.includes(abi), 'Known Android ABI required');
  const binding = {schema: 1, target: `android-${abi}`, ...await identity(buildDirectory), binaries: []};
  for (const [name, file] of Object.entries(names)) {
    const bytes = await readFile(path.join(buildDirectory, file));
    assert(bytes.length > 10000, `Invalid completed Android output ${name}`);
    binding.binaries.push({name, file, bytes: bytes.length, sha256: digest(bytes)});
  }
  await writeFile(path.join(buildDirectory, receiptName), `${JSON.stringify(binding, null, 2)}\n`);
  return binding;
}

export function assertAndroidArtifactBinding(binding, name, bytes) {
  assert.equal(binding?.schema, 1, 'Completed Android build binding required');
  assert(Object.hasOwn(names, name), 'Known Android binary required');
  assert(Array.isArray(binding.binaries) && binding.binaries.length === 3);
  assert.deepEqual(binding.binaries.map(row => row.name).sort(), Object.keys(names).sort());
  const entry = binding.binaries.find(row => row.name === name);
  assert.equal(entry.file, names[name]);
  assert.equal(bytes.length, entry.bytes, `Android staged ${name} size differs from the completed build`);
  assert.equal(digest(bytes), entry.sha256, `Android staged ${name} digest differs from the completed build`);
}

export async function readAndroidBuildBinding(buildDirectory, {abi, gitSha, sourceTree, packageVersion}) {
  assert(abis.includes(abi), 'Known Android ABI required');
  const binding = JSON.parse(await readFile(path.join(buildDirectory, receiptName), 'utf8'));
  assert.equal(binding.schema, 1);
  assert.equal(binding.target, `android-${abi}`);
  const current = await identity(buildDirectory);
  assert.equal(binding.headerSha256, current.headerSha256, 'Android header changed after completed build');
  assert.deepEqual(binding.build, current.build, 'Android binding identity differs from its header');
  assert.equal(binding.build.gitSha, gitSha);
  assert.equal(binding.build.sourceTree, sourceTree);
  assert.equal(binding.build.sourceDirty, false, 'Android release inputs require clean source');
  assert.equal(binding.build.packageVersion, packageVersion);
  for (const [name, file] of Object.entries(names))
    assertAndroidArtifactBinding(binding, name, await readFile(path.join(buildDirectory, file)));
  return binding;
}
