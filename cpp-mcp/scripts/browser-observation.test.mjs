import assert from 'node:assert/strict';
import test from 'node:test';
import { settleBrowserObservation } from './browser-observation.mjs';

const observation = (id, status, picture = true) => ({
  isError: false,
  structuredContent: { data: { session_id: 'session', observation_id: id, observed_at: id,
    page_status: status } },
  content: picture ? [{ type: 'image', mimeType: 'image/jpeg', data: id }] : [],
});

test('a tool error retains the initial image and marks the failed observation', async () => {
  const initial = observation('first', 'loading');
  const result = await settleBrowserObservation(initial, async (name, args) => {
    assert.equal(name, 'devbox_browser');
    assert.deepEqual(args, { action: 'observe', session_id: 'session', wait_ms: 1500, include_html: true });
    return { isError: true, structuredContent: { summary: 'BROWSER_PROTOCOL_DEADLINE' }, content: [] };
  });
  assert.equal(result.response, initial);
  assert.equal(result.image.data, 'first');
  assert.equal(result.imageObservationId, 'first');
  assert.equal(result.error, 'BROWSER_PROTOCOL_DEADLINE');
});

test('a transport failure retains the most recent successful image, not the first', async () => {
  let calls = 0;
  const result = await settleBrowserObservation(observation('first', 'loading'), async () => {
    if (++calls === 1) return observation('second', 'navigation_unstable');
    throw new Error('transport closed');
  });
  assert.equal(result.image.data, 'second');
  assert.equal(result.data.observation_id, 'second');
  assert.match(result.error, /transport closed/);
  assert.equal(result.observations.length, 2);
});

test('successful content stops observation and retains the image timestamp', async () => {
  let calls = 0;
  const result = await settleBrowserObservation(observation('first', 'loading'), async () => {
    calls++;
    return observation('ready', 'content_available');
  });
  assert.equal(calls, 1);
  assert.equal(result.error, null);
  assert.equal(result.imageObservedAt, 'ready');
});

test('a later image-free response does not discard an earlier picture', async () => {
  const result = await settleBrowserObservation(observation('first', 'loading'), async () =>
    observation('ready', 'content_available', false));
  assert.equal(result.data.observation_id, 'ready');
  assert.equal(result.imageObservationId, 'first');
  assert.equal(result.image.data, 'first');
});

test('already-readable pages do not poll and loading pages have a four-call bound', async () => {
  await settleBrowserObservation(observation('ready', 'content_available'), async () => assert.fail('unexpected poll'));
  let calls = 0;
  const result = await settleBrowserObservation(observation('first', 'loading'), async () =>
    observation(String(++calls), 'loading'));
  assert.equal(calls, 4);
  assert.equal(result.image.data, '4');
  assert.equal(result.data.page_status, 'loading');
});
