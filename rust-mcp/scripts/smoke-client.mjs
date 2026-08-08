import assert from "node:assert/strict";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const baseUrl = new URL(process.env.RUST_MCP_URL || "http://127.0.0.1:18182/");
const expectedTools = [
  "devbox_status",
  "devbox_wait",
  "devbox_wait_for_file",
  "host_status",
  "windows_host_status",
].sort();

const healthResponse = await fetch(new URL("healthz", baseUrl));
assert.equal(healthResponse.status, 200);
assert.equal(await healthResponse.text(), "ok");

const metadataResponse = await fetch(baseUrl);
assert.equal(metadataResponse.status, 200);
const metadata = await metadataResponse.json();
assert.equal(metadata.implementation, "rust");
assert.equal(metadata.rust_replacement?.draft, true);
assert.equal(metadata.rust_replacement?.parity?.target_count, 37);
assert.equal(metadata.rust_replacement?.parity?.implemented_count, expectedTools.length);

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
