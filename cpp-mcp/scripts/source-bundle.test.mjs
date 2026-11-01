import test from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtemp, mkdir, readFile, realpath, rm, writeFile} from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {createCertificationBundle} from './source-bundle.mjs';

const generator = fileURLToPath(new URL('../cmake/generate_identity.cmake', import.meta.url));
const tag = 'bootstrap-v0.9.23';
async function fixture(run) {
  const parent = await realpath(os.tmpdir());
  const root = await mkdtemp(path.join(parent, 'devbox-source-bundle-'));
  const source = path.join(root, 'source with spaces');
  await mkdir(path.join(source, 'cpp-bootstrap'), {recursive: true});
  const invoke = (cwd, exe, args) => {
    const result = spawnSync(exe, args, {cwd, encoding: 'utf8', timeout: 20000, windowsHide: true,
      env: {...process.env, GIT_CONFIG_NOSYSTEM: '1',
        GIT_CONFIG_GLOBAL: process.platform === 'win32' ? 'NUL' : '/dev/null'}});
    assert.equal(result.status, 0, result.stderr || result.stdout || String(result.error));
    return result.stdout.trim();
  };
  const git = (...args) => invoke(source, 'git', ['-c', 'user.name=Devbox fixture',
    '-c', 'user.email=fixture@invalid.test', ...args]);
  const identity = async (clone, name) => {
    const output = path.join(root, name);
    invoke(root, 'cmake', [`-DSOURCE_ROOT=${clone}`, `-DOUTPUT_DIR=${output}`,
      '-DCOMPILER=fixture', '-DVERSION=0.3.0', '-P', generator]);
    const header = await readFile(path.join(output, 'build_identity.hpp'), 'utf8');
    return Object.fromEntries([...header.matchAll(/const char\* (\w+) = "([^"]*)";/g)]
      .map(match => [match[1], match[2]]));
  };
  try {
    await writeFile(path.join(source, 'cpp-bootstrap/VERSION'), '0.9.23\n');
    git('init', '--quiet'); git('add', '.'); git('commit', '--quiet', '-m', 'fixture');
    await run({root, source, git, invoke, identity});
  } finally {
    assert.equal(path.dirname(await realpath(root)), parent);
    assert.equal(await realpath(root), root);
    await rm(root, {recursive: true, force: true});
  }
}

for (const annotated of [false, true]) test(`isolated bundle retains ${annotated ? 'annotated' : 'lightweight'} release identity`, async () => {
  await fixture(async ({root, source, git, invoke, identity}) => {
    git('tag', ...(annotated ? ['-a', tag, '-m', 'fixture tag'] : [tag]));
    git('checkout', '--quiet', '--detach');
    const sha = git('rev-parse', 'HEAD');
    const oldBundle = path.join(root, 'head-only.bundle');
    git('bundle', 'create', oldBundle, 'HEAD');
    const oldClone = path.join(root, 'old clone');
    invoke(root, 'git', ['clone', '--quiet', oldBundle, oldClone]);
    const oldIdentity = await identity(oldClone, 'old-generated');
    assert.equal(oldIdentity.sha, sha);
    assert.equal(oldIdentity.release_tag, '', 'The old HEAD-only transfer reproduces the missing release tag');

    const bundle = path.join(root, 'qualified.bundle');
    assert.deepEqual(await createCertificationBundle(source, bundle, tag), {source: sha, releaseTag: tag});
    const clone = path.join(root, 'new clone');
    invoke(root, 'git', ['clone', '--quiet', bundle, clone]);
    const retained = await identity(clone, 'new-generated');
    assert.equal(retained.sha, sha); assert.equal(retained.tree, oldIdentity.tree);
    assert.equal(retained.dirty, 'false'); assert.equal(retained.release_tag, tag);
  });
});

test('missing, stale and mismatched release refs fail before a tagged build', async () => {
  await fixture(async ({root, source, git, invoke, identity}) => {
    await assert.rejects(createCertificationBundle(source, path.join(root, 'missing.bundle'), tag), /exact source commit/u);
    await assert.rejects(createCertificationBundle(source, path.join(root, 'wrong.bundle'), 'bootstrap-v0.9.24'), /source package version/u);
    git('tag', tag);
    await writeFile(path.join(source, 'next.txt'), 'later clean source');
    git('add', '.'); git('commit', '--quiet', '-m', 'next commit');
    await assert.rejects(createCertificationBundle(source, path.join(root, 'stale.bundle'), tag), /exact source commit/u);
    const bundle = path.join(root, 'untagged.bundle');
    const result = await createCertificationBundle(source, bundle);
    assert.equal(result.releaseTag, null);
    const clone = path.join(root, 'untagged clone');
    invoke(root, 'git', ['clone', '--quiet', bundle, clone]);
    assert.equal((await identity(clone, 'untagged-generated')).release_tag, '');
    await writeFile(path.join(source, 'dirty.txt'), 'uncommitted input');
    await assert.rejects(createCertificationBundle(source, path.join(root, 'dirty.bundle')), /clean source/u);
  });
});
