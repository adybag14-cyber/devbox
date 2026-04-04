import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, open, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const sha256Buffer = (value) => createHash("sha256").update(value).digest("hex");

export const hashFileSha256 = async (filePath) =>
  new Promise((resolve, reject) => {
    const hash = createHash("sha256");
    const stream = createReadStream(filePath);
    stream.on("error", reject);
    stream.on("data", (chunk) => hash.update(chunk));
    stream.on("end", () => resolve(hash.digest("hex")));
  });

export const decodeBase64Payload = (value) => {
  const normalized = String(value ?? "").replace(/\s+/g, "");
  if (normalized.length === 0) {
    return Buffer.alloc(0);
  }

  if (normalized.length % 4 !== 0 || /[^A-Za-z0-9+/=]/.test(normalized)) {
    throw new Error("Invalid base64 payload.");
  }

  const decoded = Buffer.from(normalized, "base64");
  if (decoded.toString("base64") !== normalized) {
    throw new Error("Invalid base64 payload.");
  }

  return decoded;
};

export const encodeUtf8Base64 = (value) => Buffer.from(String(value), "utf8").toString("base64");

export const normalizeLargeWritePayload = ({ content, contentBase64 }) => {
  const hasText = content !== undefined;
  const hasBase64 = contentBase64 !== undefined;

  if (hasText && hasBase64) {
    throw new Error("Provide either content or content_base64, not both.");
  }

  if (!hasText && !hasBase64) {
    throw new Error("Either content or content_base64 is required.");
  }

  return hasBase64 ? String(contentBase64) : encodeUtf8Base64(content);
};

export const summarizeLargeReadData = (data) => ({
  path: data.path,
  file_size: data.file_size,
  offset_bytes_requested: data.offset_bytes_requested,
  offset_bytes: data.offset_bytes,
  bytes_requested: data.bytes_requested,
  bytes_returned: data.bytes_returned,
  next_offset_bytes: data.next_offset_bytes,
  eof: data.eof,
  content_sha256: data.content_sha256,
  content_base64_chars: data.content_base64.length,
});

export const summarizeLargeWriteData = (data) => ({
  path: data.path,
  append: data.append,
  previous_file_size: data.previous_file_size,
  final_file_size: data.final_file_size,
  bytes_written: data.bytes_written,
  content_sha256: data.content_sha256,
  verification_mode: data.verification_mode,
  verified: data.verified,
  expected_sha256_verified: data.expected_sha256_verified,
  target_existed: data.target_existed,
});

export const readLargeFileChunk = async ({ path: filePath, offsetBytes = 0, maxBytes = 262144 }) => {
  const fileStat = await stat(filePath);
  if (!fileStat.isFile()) {
    throw new Error("Not a regular file.");
  }

  const offsetRequested = Math.max(0, Number(offsetBytes) || 0);
  const bytesRequested = Math.max(1, Number(maxBytes) || 0);
  const actualOffset = Math.min(offsetRequested, fileStat.size);
  const bytesToRead = Math.max(0, Math.min(bytesRequested, fileStat.size - actualOffset));

  const handle = await open(filePath, "r");
  try {
    const buffer = Buffer.alloc(bytesToRead);
    const { bytesRead } =
      bytesToRead === 0 ? { bytesRead: 0 } : await handle.read(buffer, 0, bytesToRead, actualOffset);
    const chunk = buffer.subarray(0, bytesRead);

    return {
      path: filePath,
      file_size: fileStat.size,
      offset_bytes_requested: offsetRequested,
      offset_bytes: actualOffset,
      bytes_requested: bytesRequested,
      bytes_returned: chunk.length,
      next_offset_bytes: actualOffset + chunk.length,
      eof: actualOffset + chunk.length >= fileStat.size,
      content_sha256: sha256Buffer(chunk),
      content_base64: chunk.toString("base64"),
    };
  } finally {
    await handle.close();
  }
};

export const writeLargeFileMirror = async ({
  path: filePath,
  contentBase64,
  append = false,
  createDirs = true,
  expectedSha256 = null,
}) => {
  const payload = decodeBase64Payload(contentBase64);
  const contentSha256 = sha256Buffer(payload);
  const normalizedExpectedSha256 = expectedSha256 ? String(expectedSha256).trim().toLowerCase() : "";
  if (normalizedExpectedSha256 && !/^[a-f0-9]{64}$/.test(normalizedExpectedSha256)) {
    throw new Error("expected_sha256 must be a 64-character SHA-256 hex string.");
  }
  if (normalizedExpectedSha256 && normalizedExpectedSha256 !== contentSha256) {
    throw new Error("Decoded payload SHA-256 did not match expected_sha256.");
  }

  let targetExisted = false;
  let previousFileSize = 0;
  try {
    const fileStat = await stat(filePath);
    if (!fileStat.isFile()) {
      throw new Error("Target exists but is not a regular file.");
    }
    targetExisted = true;
    previousFileSize = fileStat.size;
  } catch (error) {
    if (error?.code !== "ENOENT") {
      throw error;
    }
  }

  const parentDir = path.dirname(filePath);
  if (createDirs && parentDir && parentDir !== ".") {
    await mkdir(parentDir, { recursive: true });
  }

  await writeFile(filePath, payload, { flag: append ? "a" : "w" });

  const finalStat = await stat(filePath);
  if (!finalStat.isFile()) {
    throw new Error("Target is not a regular file after write.");
  }

  let verified = false;
  let verificationMode = "";
  let fileSha256 = null;
  if (append) {
    verificationMode = "suffix-bytes";
    if (payload.length === 0) {
      verified = true;
    } else {
      const handle = await open(filePath, "r");
      try {
        const verifyBuffer = Buffer.alloc(payload.length);
        const { bytesRead } = await handle.read(verifyBuffer, 0, payload.length, previousFileSize);
        verified = bytesRead === payload.length && verifyBuffer.equals(payload);
      } finally {
        await handle.close();
      }
    }
  } else {
    verificationMode = "whole-file-sha256";
    fileSha256 = await hashFileSha256(filePath);
    verified = finalStat.size === payload.length && fileSha256 === contentSha256;
  }

  if (!verified) {
    throw new Error("Mirror verification failed after writing the payload.");
  }

  return {
    path: filePath,
    append: Boolean(append),
    previous_file_size: previousFileSize,
    final_file_size: finalStat.size,
    bytes_written: payload.length,
    content_sha256: contentSha256,
    verification_mode: verificationMode,
    verified,
    expected_sha256_verified: normalizedExpectedSha256 ? true : null,
    target_existed: targetExisted,
    file_sha256: fileSha256,
  };
};

const readStdinUtf8 = async () => {
  const chunks = [];
  for await (const chunk of process.stdin) {
    chunks.push(typeof chunk === "string" ? Buffer.from(chunk) : chunk);
  }
  return Buffer.concat(chunks).toString("utf8");
};

export const runLargeFileCli = async (argv) => {
  const [command, ...args] = argv;
  if (command === "read") {
    const [filePath, offsetBytes = "0", maxBytes = "262144"] = args;
    if (!filePath) {
      throw new Error("Usage: node src/large-file-cli.js read <path> <offset_bytes> <max_bytes>");
    }
    const result = await readLargeFileChunk({
      path: filePath,
      offsetBytes: Number(offsetBytes),
      maxBytes: Number(maxBytes),
    });
    process.stdout.write(JSON.stringify(result));
    return;
  }

  if (command === "write") {
    const [filePath, append = "0", createDirs = "1", expectedSha256 = ""] = args;
    if (!filePath) {
      throw new Error(
        "Usage: node src/large-file-cli.js write <path> <append0or1> <create_dirs0or1> [expected_sha256]",
      );
    }
    const stdinBase64 = await readStdinUtf8();
    const result = await writeLargeFileMirror({
      path: filePath,
      contentBase64: stdinBase64,
      append: append === "1",
      createDirs: createDirs === "1",
      expectedSha256: expectedSha256 || null,
    });
    process.stdout.write(JSON.stringify(result));
    return;
  }

  throw new Error("Usage: node src/large-file-cli.js <read|write> ...");
};

const isMainModule = process.argv[1] === fileURLToPath(import.meta.url);

if (isMainModule) {
  runLargeFileCli(process.argv.slice(2)).catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  });
}
