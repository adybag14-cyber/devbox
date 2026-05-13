import test from "node:test";
import assert from "node:assert/strict";

import { spawnProcess, trimText } from "../src/process-utils.js";

test("trimText does not truncate when the character limit is disabled", () => {
  const text = "abcdef";

  assert.deepEqual(trimText(text, null), { text, truncated: false });
  assert.deepEqual(trimText(text, undefined), { text, truncated: false });
  assert.deepEqual(trimText(text, Infinity), { text, truncated: false });
});

test("trimText still truncates finite character limits", () => {
  const result = trimText("abcdefghijklmnopqrstuvwxyz", 16);

  assert.equal(result.truncated, true);
  assert.match(result.text, /truncated to 16 characters/);
  assert.ok(result.text.length > 0);
});

test("spawnProcess rejects promptly when a child ignores graceful timeout termination", async () => {
  const startedAt = Date.now();

  await assert.rejects(
    () =>
      spawnProcess(
        process.execPath,
        ["-e", "process.on('SIGTERM', () => {}); setInterval(() => {}, 1000);"],
        { timeoutMs: 100 },
      ),
    (error) => {
      assert.match(error.message, /Command timed out after 100 ms\./);
      return true;
    },
  );

  assert.ok(Date.now() - startedAt < 5000);
});
