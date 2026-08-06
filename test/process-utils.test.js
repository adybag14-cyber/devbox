import test from "node:test";
import assert from "node:assert/strict";

import {
  MAX_PROCESS_ERROR_MESSAGE_CHARS,
  spawnProcess,
  summarizeProcessFailure,
  trimText,
} from "../src/process-utils.js";

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

test("spawnProcess cancels a running child when the request signal aborts", async () => {
  const controller = new AbortController();
  const pending = spawnProcess(
    process.execPath,
    ["-e", "setInterval(() => {}, 1000);"],
    { signal: controller.signal, timeoutRejectGraceMs: 100 },
  );
  controller.abort();

  await assert.rejects(pending, (error) => {
    assert.equal(error.aborted, true);
    assert.match(error.message, /cancelled by the MCP client/u);
    return true;
  });
});

test("nonzero commands keep full stdout separately without copying it into the error summary", async () => {
  const outputLength = MAX_PROCESS_ERROR_MESSAGE_CHARS * 4;

  await assert.rejects(
    spawnProcess(process.execPath, ["-e", `process.stdout.write('x'.repeat(${outputLength})); process.exit(7);`]),
    (error) => {
      assert.equal(error.exitCode, 7);
      assert.equal(error.stdout.length, outputLength);
      assert.ok(error.message.length <= MAX_PROCESS_ERROR_MESSAGE_CHARS);
      assert.match(error.message, /producing 16384 characters of stdout/u);
      return true;
    },
  );
});

test("stderr-derived process summaries are bounded", () => {
  const message = summarizeProcessFailure({
    file: "tool",
    code: 9,
    stderr: "failure".repeat(MAX_PROCESS_ERROR_MESSAGE_CHARS),
  });

  assert.ok(message.length <= MAX_PROCESS_ERROR_MESSAGE_CHARS);
  assert.match(message, /error summary truncated/u);
});

test("spawnProcess streams full output while bounding in-memory capture for background jobs", async () => {
  let streamedStdout = "";
  let streamedStderr = "";
  const result = await spawnProcess(
    process.execPath,
    ["-e", "process.stdout.write('abcdefgh'); process.stderr.write('12345678');"],
    {
      maxCaptureChars: 4,
      onStdout: (text) => { streamedStdout += text; },
      onStderr: (text) => { streamedStderr += text; },
    },
  );

  assert.equal(streamedStdout, "abcdefgh");
  assert.equal(streamedStderr, "12345678");
  assert.equal(result.stdout, "efgh");
  assert.equal(result.stderr, "5678");
});
