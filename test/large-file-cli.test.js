import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, readFile, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import {
  decodeBase64Payload,
  encodeUtf8Base64,
  normalizeLargeWritePayload,
  readLargeFileChunk,
  writeLargeFileMirror,
} from "../src/large-file-cli.js";

const makeTempDir = () => mkdtemp(path.join(os.tmpdir(), "devbox-large-file-"));

test("readLargeFileChunk returns exact base64 bytes and paging metadata", async () => {
  const tempDir = await makeTempDir();
  const filePath = path.join(tempDir, "sample.bin");
  const payload = Buffer.from([0x00, 0x01, 0x02, 0xff, 0xfe, 0xfd, 0x41, 0x42, 0x43]);
  await writeFile(filePath, payload);

  const result = await readLargeFileChunk({ path: filePath, offsetBytes: 2, maxBytes: 4 });

  assert.equal(result.file_size, payload.length);
  assert.equal(result.offset_bytes_requested, 2);
  assert.equal(result.offset_bytes, 2);
  assert.equal(result.bytes_requested, 4);
  assert.equal(result.bytes_returned, 4);
  assert.equal(result.next_offset_bytes, 6);
  assert.equal(result.eof, false);
  assert.deepEqual(decodeBase64Payload(result.content_base64), payload.subarray(2, 6));
});

test("readLargeFileChunk clamps later offsets to EOF and returns an empty chunk", async () => {
  const tempDir = await makeTempDir();
  const filePath = path.join(tempDir, "tail.bin");
  const payload = Buffer.from("abcdef", "utf8");
  await writeFile(filePath, payload);

  const result = await readLargeFileChunk({ path: filePath, offsetBytes: 999, maxBytes: 64 });

  assert.equal(result.offset_bytes, payload.length);
  assert.equal(result.bytes_returned, 0);
  assert.equal(result.next_offset_bytes, payload.length);
  assert.equal(result.eof, true);
  assert.equal(result.content_base64, "");
});

test("writeLargeFileMirror writes exact bytes and verifies full-file hash", async () => {
  const tempDir = await makeTempDir();
  const filePath = path.join(tempDir, "mirror.bin");
  const payload = Buffer.from([0x00, 0x7f, 0x80, 0xff, 0x10, 0x20, 0x30]);

  const result = await writeLargeFileMirror({
    path: filePath,
    contentBase64: payload.toString("base64"),
  });

  const written = await readFile(filePath);
  assert.deepEqual(written, payload);
  assert.equal(result.bytes_written, payload.length);
  assert.equal(result.previous_file_size, 0);
  assert.equal(result.final_file_size, payload.length);
  assert.equal(result.verification_mode, "whole-file-sha256");
  assert.equal(result.verified, true);
  assert.equal(result.content_sha256, result.file_sha256);
});

test("writeLargeFileMirror appends exact suffix bytes and verifies them", async () => {
  const tempDir = await makeTempDir();
  const filePath = path.join(tempDir, "append.bin");
  await writeFile(filePath, Buffer.from("hello", "utf8"));
  const suffix = Buffer.from([0x00, 0x01, 0x02, 0xff]);

  const result = await writeLargeFileMirror({
    path: filePath,
    contentBase64: suffix.toString("base64"),
    append: true,
  });

  const written = await readFile(filePath);
  assert.deepEqual(written, Buffer.concat([Buffer.from("hello", "utf8"), suffix]));
  assert.equal(result.previous_file_size, 5);
  assert.equal(result.final_file_size, 9);
  assert.equal(result.verification_mode, "suffix-bytes");
  assert.equal(result.verified, true);
});

test("writeLargeFileMirror rejects invalid base64 payloads", async () => {
  const tempDir = await makeTempDir();
  const filePath = path.join(tempDir, "bad.bin");

  await assert.rejects(
    () =>
      writeLargeFileMirror({
        path: filePath,
        contentBase64: "not-base64!!!",
      }),
    /Invalid base64 payload/,
  );
});

test("writeLargeFileMirror rejects mismatched expected_sha256", async () => {
  const tempDir = await makeTempDir();
  const filePath = path.join(tempDir, "sha.bin");
  const payload = Buffer.from("sha-check", "utf8");

  await assert.rejects(
    () =>
      writeLargeFileMirror({
        path: filePath,
        contentBase64: payload.toString("base64"),
        expectedSha256: "0".repeat(64),
      }),
    /expected_sha256/,
  );
});

test("readLargeFileChunk rejects non-regular files", async () => {
  const tempDir = await makeTempDir();
  const dirPath = path.join(tempDir, "folder");
  await mkdir(dirPath);

  await assert.rejects(() => readLargeFileChunk({ path: dirPath }), /Not a regular file/);
});

test("normalizeLargeWritePayload encodes text and preserves explicit base64", async () => {
  assert.equal(normalizeLargeWritePayload({ content: "hello" }), encodeUtf8Base64("hello"));
  assert.equal(normalizeLargeWritePayload({ contentBase64: "aGVsbG8=" }), "aGVsbG8=");

  assert.throws(
    () => normalizeLargeWritePayload({ content: "hello", contentBase64: "aGVsbG8=" }),
    /either content or content_base64, not both/i,
  );

  assert.throws(() => normalizeLargeWritePayload({}), /Either content or content_base64 is required/);
});
