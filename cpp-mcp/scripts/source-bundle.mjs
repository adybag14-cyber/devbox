import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';

// Carry the same clean source identity into an isolated build. Only the exact
// matching package tag is exported; an older/unrelated tag cannot label HEAD.
export async function createCertificationBundle(repo, destination, requiredTag = '') {
  const git = async (...args) => (await runCheckedProcess('git', ['-C', repo, ...args],
    {cwd: repo, timeoutMs: 30000, label: 'Native certification source bundle'})).stdout.trim();
  const source = await git('rev-parse', 'HEAD');
  assert.match(source, /^[a-f0-9]{40}$/u);
  assert.equal(await git('status', '--porcelain', '--untracked-files=all'), '',
    'Certification bundles require clean source');
  const version = (await readFile(path.join(repo, 'cpp-bootstrap/VERSION'), 'utf8')).trim();
  assert.match(version, /^\d+\.\d+\.\d+$/u);
  const name = `bootstrap-v${version}`, ref = `refs/tags/${name}`;
  if (requiredTag) assert.equal(requiredTag, name, 'Release tag must match the source package version');
  const refs = (await git('for-each-ref', '--format=%(refname)', ref)).split('\n');
  const matching = refs.includes(ref) && await git('rev-parse', '--verify', `${ref}^{commit}`) === source;
  if (requiredTag) assert(matching, 'The requested release tag must name this exact source commit');
  await git('bundle', 'create', destination, 'HEAD', ...(matching ? [ref] : []));
  return {source, releaseTag: matching ? name : null};
}
