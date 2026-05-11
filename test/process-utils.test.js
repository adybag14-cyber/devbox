import test from "node:test";
import assert from "node:assert/strict";

import { trimText } from "../src/process-utils.js";

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
