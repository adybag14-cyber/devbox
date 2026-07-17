import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, writeFile } from "node:fs/promises";

import { loadEnvFile, parseEnvText } from "../src/env.js";

test("parseEnvText handles comments, quotes, export, and empty values", () => {
  assert.deepEqual(
    parseEnvText(`
# comment
PLAIN=value
QUOTED="hello world"
SINGLE='literal value'
export EXPORTED=yes
EMPTY=
INLINE=value # comment
`),
    {
      PLAIN: "value",
      QUOTED: "hello world",
      SINGLE: "literal value",
      EXPORTED: "yes",
      EMPTY: "",
      INLINE: "value",
    },
  );
});

test("loadEnvFile preserves existing environment values", async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "devbox-env-"));
  const filePath = path.join(tempDir, ".env");
  await writeFile(filePath, "EXISTING=file\nNEW_VALUE=loaded\n", "utf8");
  const env = { EXISTING: "process" };

  const result = await loadEnvFile(filePath, env);

  assert.equal(result.loaded, true);
  assert.equal(env.EXISTING, "process");
  assert.equal(env.NEW_VALUE, "loaded");
  assert.deepEqual(result.keys, ["NEW_VALUE"]);
});
