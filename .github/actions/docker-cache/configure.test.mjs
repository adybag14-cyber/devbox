import assert from 'node:assert/strict';
import test from 'node:test';
import { withPublicMirror } from './configure.mjs';

test('adds the public cache without dropping daemon settings or existing mirrors', () => {
  const original = { 'live-restore': true, features: { buildkit: true },
    'registry-mirrors': ['https://existing.example', 'https://mirror.gcr.io/'] };
  const saved = structuredClone(original);
  const updated = withPublicMirror(original);
  assert.deepEqual(original, saved);
  assert.deepEqual(updated, { 'live-restore': true, features: { buildkit: true },
    'registry-mirrors': ['https://mirror.gcr.io', 'https://existing.example'] });
  assert.deepEqual(withPublicMirror(updated), updated);
});

test('initializes an absent mirror list and rejects malformed configuration', () => {
  assert.deepEqual(withPublicMirror({}), { 'registry-mirrors': ['https://mirror.gcr.io'] });
  for (const invalid of [null, [], { 'registry-mirrors': null }, { 'registry-mirrors': 'invalid' }, { 'registry-mirrors': [1] }]) {
    assert.throws(() => withPublicMirror(invalid));
  }
});
