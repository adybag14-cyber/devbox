import assert from "node:assert/strict";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const baseUrl = new URL(process.env.RUST_MCP_URL || "http://127.0.0.1:18182/");


const healthResponse = await fetch(new URL("healthz", baseUrl));
assert.equal(healthResponse.status, 200);
assert.equal(await healthResponse.text(), "ok");

const metadataResponse = await fetch(baseUrl);
assert.equal(metadataResponse.status, 200);
const metadata = await metadataResponse.json();
assert.equal(metadata.implementation, "rust");
assert.equal(metadata.rust_replacement?.draft, true);
assert.equal(metadata.rust_replacement?.parity?.target_count, 37);
const expectedTools = [...(metadata.rust_replacement?.parity?.implemented || [])].sort();
assert.equal(metadata.rust_replacement?.parity?.implemented_count, expectedTools.length);
for (const requiredTool of [
  "devbox_status",
  "devbox_wait",
  "devbox_wait_for_file",
  "host_status",
  "windows_host_status",
  "windows_host_read_large_file",
  "windows_host_write_large_file",
]) {
  assert.ok(expectedTools.includes(requiredTool), `parity report omitted established tool ${requiredTool}`);
}

const transport = new StreamableHTTPClientTransport(baseUrl);
const client = new Client({ name: "rust-parity-smoke", version: "0.1.0" });
try {
  await client.connect(transport);
  const listed = await client.listTools();
  const names = listed.tools.map((tool) => tool.name).sort();
  assert.deepEqual(names, expectedTools);

  const wait = await client.callTool({
    name: "devbox_wait",
    arguments: { seconds: 0.05, reason: "rust-shadow-smoke" },
  });
  assert.equal(wait.isError, false);
  assert.equal(wait.structuredContent?.ok, true);
  assert.equal(wait.structuredContent?.data?.reason, "rust-shadow-smoke");
  assert.ok(wait.structuredContent?.data?.waited_ms >= 40);

  const status = await client.callTool({ name: "devbox_status", arguments: {} });
  assert.equal(status.isError, false);
  assert.equal(status.structuredContent?.ok, true);
  assert.equal(status.structuredContent?.data?.rustReplacement?.implemented_count, expectedTools.length);

  const fixtureDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-mcp-smoke-"));
  const fixturePath = path.join(fixtureDir, "exact-bytes.bin");
  try {
    const write = await client.callTool({
      name: "windows_host_write_large_file",
      arguments: {
        path: fixturePath,
        content_base64: "AP9hbHBoYQo=",
        expected_sha256: "5cd13e70af539c9471799a5cf52bc04af6ee4a13bd866523861a1fc51fa6acb5",
      },
    });
    assert.equal(write.isError, false);
    assert.equal(write.structuredContent?.ok, true);
    assert.equal(write.structuredContent?.data?.bytes_written, 8);
    assert.equal(write.structuredContent?.data?.verified, true);
    assert.deepEqual(await readFile(fixturePath), Buffer.from([0x00, 0xff, 0x61, 0x6c, 0x70, 0x68, 0x61, 0x0a]));

    const read = await client.callTool({
      name: "windows_host_read_large_file",
      arguments: { path: fixturePath, offset_bytes: 1, max_bytes: 4 },
    });
    assert.equal(read.isError, false);
    assert.equal(read.structuredContent?.ok, true);
    assert.equal(read.structuredContent?.data?.content_base64, "/2FscA==");
    assert.equal(read.structuredContent?.data?.bytes_returned, 4);
    assert.equal(read.structuredContent?.data?.next_offset_bytes, 5);
    assert.ok(!read.content?.[0]?.text?.includes("/2FscA=="), "raw base64 must not be duplicated into MCP text content");
  } finally {
    await rm(fixtureDir, { recursive: true, force: true });
  }

  console.log(JSON.stringify({
    ok: true,
    baseUrl: baseUrl.toString(),
    tools: names,
    parity: metadata.rust_replacement.parity,
    waitMs: wait.structuredContent.data.waited_ms,
  }, null, 2));
} finally {
  await client.close();
}
