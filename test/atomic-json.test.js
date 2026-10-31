import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, readFile, readdir, realpath, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { writeJsonAtomic } from "../src/atomic-json.js";

test("concurrent atomic JSON writes remain complete and preserve call order", async () => {
  const parent = await realpath(os.tmpdir());
  const root = await mkdtemp(path.join(parent, "devbox-atomic-json-"));
  try {
    const file = path.join(root, "state.json");
    await writeJsonAtomic(file, {index: -1, payload: "initial"});
    const pending = Array.from({length: 80}, (_, index) => writeJsonAtomic(file, {index, payload: "é😀".repeat(500)}));
    await Promise.all([...pending, (async () => {
      for (let index = 0; index < 40; index += 1) {
        const state = JSON.parse(await readFile(file, "utf8"));
        assert(Number.isInteger(state.index));
        assert.equal(state.payload, state.index === -1 ? "initial" : "é😀".repeat(500));
      }
    })()]);
    assert.equal(JSON.parse(await readFile(file, "utf8")).index, 79);
    await assert.rejects(writeJsonAtomic(file, {invalid: 1n}), TypeError);
    assert.equal(JSON.parse(await readFile(file, "utf8")).index, 79);
    await writeJsonAtomic(file, {index: 80});
    assert.deepEqual(JSON.parse(await readFile(file, "utf8")), {index: 80});
    assert.deepEqual(await readdir(root), ["state.json"]);
  } finally {
    assert.equal(path.dirname(await realpath(root)), parent);
    assert.equal(await realpath(root), root);
    await rm(root, {recursive: true, force: true});
  }
});
