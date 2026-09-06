import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const readme = readFileSync(new URL('../README.md', import.meta.url), 'utf8');
const snippet = readme.match(/<!-- devbox-auto-continue:start -->\s*```javascript\n([\s\S]*?)\n```/)[1];

function fixture(options = {}) {
  let now = 0;
  let clicks = 0;
  let inserts = 0;
  const intervals = new Map();
  const waits = [];
  let nextId = 0;
  const element = (extra = {}) => ({
    isConnected: true, parentElement: null,
    style: { visibility: 'visible', opacity: '1', display: 'block' },
    getClientRects: () => [1],
    getBoundingClientRect: () => ({ top: 1, left: 1, bottom: 20, right: 20 }),
    ...extra,
  });
  const button = element({ disabled: !!options.disabled, getAttribute: () => null, click() { clicks++; box.textContent = ''; } });
  const form = element({ querySelectorAll: () => options.noButton ? [] : [button] });
  const box = element({
    textContent: options.draft || '', isContentEditable: true,
    focus() {}, closest: () => options.noForm ? null : form,
  });
  const boxes = options.ambiguous ? [box, element({ isContentEditable: true })] : [box];
  const document = {
    visibilityState: 'visible',
    querySelectorAll: selector => selector === '[contenteditable]' ? boxes : (options.generating ? [element()] : []),
    createRange: () => ({ selectNodeContents() {}, collapse() {} }),
    execCommand() {
      inserts++;
      if (options.throws) throw new Error('insertion rejected');
      if (options.rejectInsertion) return false;
      box.textContent = 'continue';
      return true;
    },
  };
  const window = { getSelection: () => ({ removeAllRanges() {}, addRange() {} }) };
  const context = vm.createContext({
    window, document, location: { href: 'https://example.test/chat/one' },
    innerWidth: 1024, innerHeight: 768, getComputedStyle: el => el.style,
    Date: { now: () => now }, console: { warn() {} },
    setInterval: fn => { const id = ++nextId; intervals.set(id, fn); return id; },
    clearInterval: id => intervals.delete(id),
    setTimeout: fn => { waits.push(fn); return ++nextId; },
  });
  const run = () => vm.runInContext(snippet, context);
  const advance = async (ms = 100) => { now += ms; waits.splice(0).forEach(fn => fn()); await Promise.resolve(); await Promise.resolve(); };
  return { context, window, document, box, button, run, advance, intervals, get clicks() { return clicks; }, get inserts() { return inserts; } };
}

test('README helper sends once from an empty associated composer and stops its timer', async () => {
  const f = fixture(); f.run(); await f.advance();
  assert.equal(f.clicks, 1); assert.equal(f.inserts, 1);
  f.window.devboxContinue.stop();
  assert.equal(f.intervals.size, 0);
  await f.window.devboxContinue.tick(); assert.equal(f.clicks, 1);
});

for (const options of [{ draft: 'unfinished message' }, { ambiguous: true }, { noForm: true }, { noButton: true }, { generating: true }]) {
  test(`README helper leaves unsupported or busy composer untouched: ${JSON.stringify(options)}`, async () => {
    const f = fixture(options); f.run(); await f.advance();
    assert.equal(f.inserts, 0); assert.equal(f.clicks, 0);
  });
}

for (const options of [{ rejectInsertion: true }, { throws: true }]) {
  test(`README helper never submits after failed insertion: ${JSON.stringify(options)}`, async () => {
    const f = fixture(options); f.run(); await f.advance(); assert.equal(f.clicks, 0);
  });
}

test('README helper polls button readiness without inserting twice', async () => {
  const f = fixture({ disabled: true }); f.run();
  await f.window.devboxContinue.tick(); await f.advance();
  assert.equal(f.inserts, 1); assert.equal(f.clicks, 0);
  f.button.disabled = false; await f.advance(); assert.equal(f.clicks, 1);
});

test('README helper abandons a user edit during readiness polling', async () => {
  const f = fixture({ disabled: true }); f.run();
  f.box.textContent = 'my revised message'; f.button.disabled = false;
  await f.advance(); assert.equal(f.clicks, 0); assert.equal(f.box.textContent, 'my revised message');
});

test('README helper cancels pending attempts on rerun and keeps one timer', async () => {
  const f = fixture({ disabled: true }); f.run(); f.run();
  f.button.disabled = false; await f.advance();
  assert.equal(f.clicks, 0); assert.equal(f.inserts, 1); assert.equal(f.intervals.size, 1);
});

test('README helper stops on navigation and cannot submit its pending attempt', async () => {
  const f = fixture({ disabled: true }); f.run(); f.context.location.href += '/other';
  f.button.disabled = false; await f.advance(); await f.window.devboxContinue.tick();
  assert.equal(f.clicks, 0); assert.equal(f.intervals.size, 0);
});

test('README helper bounds readiness waiting and preserves inserted text on timeout', async () => {
  const f = fixture({ disabled: true }); f.run(); await f.advance(5001);
  f.button.disabled = false; await f.window.devboxContinue.tick();
  assert.equal(f.clicks, 0); assert.equal(f.inserts, 1); assert.equal(f.box.textContent, 'continue');
});

for (const hidden of ['tab', 'opacity', 'viewport', 'ancestor']) {
  test(`README helper ignores invisible composers: ${hidden}`, async () => {
    const f = fixture();
    if (hidden === 'tab') f.document.visibilityState = 'hidden';
    if (hidden === 'opacity') f.box.style.opacity = '0';
    if (hidden === 'viewport') f.box.getBoundingClientRect = () => ({ top: 900, left: 1, bottom: 920, right: 20 });
    if (hidden === 'ancestor') f.box.parentElement = { parentElement: null, style: { visibility: 'hidden' } };
    f.run(); await f.advance(); assert.equal(f.inserts, 0); assert.equal(f.clicks, 0);
  });
}
