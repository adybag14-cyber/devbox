import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, mkdir, readFile, writeFile, rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {writeAndroidBuildBinding, readAndroidBuildBinding, assertAndroidArtifactBinding} from './android-artifact-binding.mjs';

const expected = {abi: 'arm64-v8a', gitSha: 'a'.repeat(40), sourceTree: 'b'.repeat(40), packageVersion: '0.9.20'};
const files = {'devbox-mcp': 'cpp-mcp/devbox-mcp', 'devbox-setup': 'cpp-bootstrap/devbox-setup', 'devbox-tui': 'setup-tui/devbox-tui'};
async function fixture(run) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'devbox-android-binding-test-'));
  try {
    const build = path.join(root, 'custom-build-directory');
    const header = path.join(build, 'cpp-mcp/generated/build_identity.hpp');
    await mkdir(path.dirname(header), {recursive: true});
    const fields = {sha: expected.gitSha, tree: expected.sourceTree, dirty: 'false', fingerprint: 'c'.repeat(64),
      compiler: 'Android Clang fixture', version: '0.3.0', package_version: expected.packageVersion, release_tag: ''};
    await writeFile(header, Object.entries(fields).map(([name, value]) =>
      `inline constexpr const char* ${name} = ${JSON.stringify(value)};`).join('\n'));
    const images = {};
    for (const [name, file] of Object.entries(files)) {
      images[name] = Buffer.alloc(12000, Object.keys(images).length + 1);
      await mkdir(path.dirname(path.join(build, file)), {recursive: true});
      await writeFile(path.join(build, file), images[name]);
    }
    await run({root, build, header, images});
  } finally {
    assert.equal(path.dirname(path.resolve(root)), path.resolve(os.tmpdir()));
    assert(path.basename(root).startsWith('devbox-android-binding-test-'));
    await rm(root, {recursive: true, force: true});
  }
}

test('Android staging accepts exact completed outputs and rejects stale files for all components', async () => {
  await fixture(async ({build, images}) => {
    await writeAndroidBuildBinding(build, expected.abi);
    const binding = await readAndroidBuildBinding(build, expected);
    assert.equal(binding.target, 'android-arm64-v8a');
    assert.match(binding.headerSha256, /^[a-f0-9]{64}$/u);
    for (const [name, bytes] of Object.entries(images)) {
      assertAndroidArtifactBinding(binding, name, bytes);
      const stale = Buffer.from(bytes); stale[stale.length - 1] ^= 1;
      assert.throws(() => assertAndroidArtifactBinding(binding, name, stale), /digest differs from the completed build/u);
      assert.throws(() => assertAndroidArtifactBinding(binding, name, bytes.subarray(1)), /size differs from the completed build/u);
    }
    assert.throws(() => assertAndroidArtifactBinding(binding, 'unrelated', images['devbox-mcp']), /Known Android binary/u);
  });
});

test('Android binding rejects changed headers, changed build outputs and a missing completion receipt', async () => {
  await fixture(async ({build, header, images}) => {
    await assert.rejects(readAndroidBuildBinding(build, expected), {code: 'ENOENT'});
    await writeAndroidBuildBinding(build, expected.abi);
    const original = await readFile(header, 'utf8');
    await writeFile(header, original.replace(expected.gitSha, 'd'.repeat(40)));
    await assert.rejects(readAndroidBuildBinding(build, expected), /header changed after completed build/u);
    await writeFile(header, original);
    const changed = Buffer.from(images['devbox-mcp']); changed[0] ^= 1;
    await writeFile(path.join(build, files['devbox-mcp']), changed);
    await assert.rejects(readAndroidBuildBinding(build, expected), /digest differs from the completed build/u);
  });
});

test('Android binding cannot be reused for another ABI, source, tree, version or dirty release', async () => {
  await fixture(async ({build, header}) => {
    await writeAndroidBuildBinding(build, expected.abi);
    for (const override of [{abi: 'x86_64'}, {gitSha: 'd'.repeat(40)}, {sourceTree: 'e'.repeat(40)}, {packageVersion: '0.9.21'}])
      await assert.rejects(readAndroidBuildBinding(build, {...expected, ...override}), assert.AssertionError);
    const original = await readFile(header, 'utf8');
    await writeFile(header, original.replace('dirty = "false"', 'dirty = "true"'));
    await writeAndroidBuildBinding(build, expected.abi); // Local diagnostic builds remain allowed.
    await assert.rejects(readAndroidBuildBinding(build, expected), /release inputs require clean source/u);
    await writeFile(header, original + '\ninline constexpr const char* sha = "' + expected.gitSha + '";');
    await assert.rejects(writeAndroidBuildBinding(build, expected.abi), /Unique generated Android identity/u);
  });
});
