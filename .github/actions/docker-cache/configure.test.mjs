import assert from 'node:assert/strict';
import test from 'node:test';
import { withPublicMirror } from './configure.mjs';
import { waitForRegistryCache } from './readiness.mjs';

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

test('a transient daemon probe timeout does not consume the remaining readiness window', async () => {
  let clock = 0, calls = 0;
  const timeouts = [];
  const version = await waitForRegistryCache('https://mirror.gcr.io', timeout => {
    timeouts.push(timeout);
    if (++calls === 1) { clock += timeout; throw new Error('owned transient probe timeout'); }
    clock += 100;
    return ['https://mirror.gcr.io/'];
  }, timeout => { timeouts.push(timeout); clock += 100; return '28.0.4\n'; },
  { now: () => clock, sleep: async ms => { clock += ms; } });
  assert.equal(version, '28.0.4');
  assert.deepEqual(timeouts, [3000, 3000, 5000]);
  assert(clock < 10000);
});

test('persistent probe failures stop at the total deadline and retain their cause', async () => {
  let clock = 0;
  const timeouts = [], failure = new Error('owned persistent daemon failure');
  await assert.rejects(waitForRegistryCache('https://mirror.gcr.io', timeout => {
    timeouts.push(timeout); clock += timeout; throw failure;
  }, () => { throw new Error('version must not be queried before mirror acknowledgement'); },
  { now: () => clock, sleep: async ms => { clock += ms; } }), error => error.cause === failure);
  assert.deepEqual(timeouts, [3000, 3000, 3000, 250]);
  assert.equal(clock, 10000);
});

test('version probing uses the remaining budget and late success is rejected', async () => {
  let clock = 0;
  await assert.rejects(waitForRegistryCache('https://mirror.gcr.io', () => {
    clock = 9999; return ['https://mirror.gcr.io'];
  }, timeout => { assert.equal(timeout, 1); clock += 2; return 'late'; },
  { now: () => clock, sleep: async ms => { clock += ms; } }), /before its deadline/);
});

test('an unrelated mirror does not qualify the daemon', async () => {
  let clock = 0, versionCalls = 0;
  await assert.rejects(waitForRegistryCache('https://mirror.gcr.io', () => ['https://unrelated.invalid'],
    () => { ++versionCalls; return '28.0.4'; },
    { now: () => clock, sleep: async ms => { clock += ms; } }), /before its deadline/);
  assert.equal(versionCalls, 0);
  assert.equal(clock, 10000);
});
