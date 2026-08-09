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
  "devbox_run_program",
  "devbox_run_program_start",
  "devbox_job_status",
  "devbox_job_logs",
  "devbox_job_cancel",
  "devbox_exec",
  "devbox_exec_readonly",
  "devbox_exec_start",
  "devbox_list_files",
  "devbox_read_file",
  "devbox_read_large_file",
  "devbox_write_file",
  "devbox_write_large_file",
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

  for (const [toolName, readOnly] of [
    ["devbox_exec", false],
    ["devbox_exec_readonly", true],
  ]) {
    const shell = await client.callTool({
      name: toolName,
      arguments: {
        command: "git --version",
        output_mode: "head",
        max_output_chars: 2_000,
      },
    });
    assert.equal(shell.isError, false);
    assert.match(shell.structuredContent?.stdout || "", /^git version /);
    assert.equal(shell.structuredContent?.data?.read_only, readOnly);
  }

  const shellStarted = await client.callTool({
    name: "devbox_exec_start",
    arguments: {
      command: "node -e \"console.log('RUST_SHELL_ASYNC_SMOKE')\"",
      timeout_seconds: 30,
      resource_class: "light",
    },
  });
  assert.equal(shellStarted.isError, false);
  const shellJobId = shellStarted.structuredContent?.data?.id;
  assert.match(shellJobId || "", /^job-/);
  const shellDone = await client.callTool({
    name: "devbox_job_status",
    arguments: { job_id: shellJobId, wait_seconds: 10, terminal_only: true },
  });
  assert.equal(shellDone.isError, false);
  assert.equal(shellDone.structuredContent?.data?.status, "succeeded");
  const shellLogs = await client.callTool({
    name: "devbox_job_logs",
    arguments: { job_id: shellJobId, max_chars: 5_000 },
  });
  assert.match(shellLogs.structuredContent?.data?.stdout || "", /RUST_SHELL_ASYNC_SMOKE/);

  const direct = await client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "git",
      args: ["--version"],
      output_mode: "head",
      max_output_chars: 2_000,
    },
  });
  assert.equal(direct.isError, false);
  assert.equal(direct.structuredContent?.ok, true);
  assert.match(direct.structuredContent?.stdout || "", /^git version /);
  assert.equal(direct.structuredContent?.data?.execution?.pool, "execution");

  const started = await client.callTool({
    name: "devbox_run_program_start",
    arguments: {
      program: "node",
      args: ["-e", "console.log('RUST_ASYNC_SMOKE')"],
      timeout_seconds: 30,
      resource_class: "light",
    },
  });
  assert.equal(started.isError, false);
  const asyncJobId = started.structuredContent?.data?.id;
  assert.match(asyncJobId || "", /^job-/);
  const asyncDone = await client.callTool({
    name: "devbox_job_status",
    arguments: { job_id: asyncJobId, wait_seconds: 10, terminal_only: true },
  });
  assert.equal(asyncDone.isError, false);
  assert.equal(asyncDone.structuredContent?.data?.status, "succeeded");
  const asyncLogs = await client.callTool({
    name: "devbox_job_logs",
    arguments: { job_id: asyncJobId, max_chars: 5_000 },
  });
  assert.equal(asyncLogs.isError, false);
  assert.match(asyncLogs.structuredContent?.data?.stdout || "", /RUST_ASYNC_SMOKE/);

  const cancellable = await client.callTool({
    name: "devbox_run_program_start",
    arguments: {
      program: "node",
      args: ["-e", "setTimeout(()=>{},60000)"],
      timeout_seconds: 60,
      resource_class: "light",
    },
  });
  assert.equal(cancellable.isError, false);
  const cancelJobId = cancellable.structuredContent?.data?.id;
  const running = await client.callTool({
    name: "devbox_job_status",
    arguments: { job_id: cancelJobId, wait_seconds: 5, terminal_only: false },
  });
  assert.equal(running.isError, false);
  assert.equal(running.structuredContent?.data?.status, "running");
  const cancel = await client.callTool({
    name: "devbox_job_cancel",
    arguments: { job_id: cancelJobId },
  });
  assert.equal(cancel.isError, false);
  assert.equal(cancel.structuredContent?.data?.status, "cancelled");
  const cancelled = await client.callTool({
    name: "devbox_job_status",
    arguments: { job_id: cancelJobId, wait_seconds: 10, terminal_only: true },
  });
  assert.equal(cancelled.isError, false);
  assert.equal(cancelled.structuredContent?.data?.status, "cancelled");

  const fixtureDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-mcp-smoke-"));
  const fixturePath = path.join(fixtureDir, "exact-bytes.bin");
  const devboxTextPath = path.join(fixtureDir, "devbox-text.txt");
  const devboxExactPath = path.join(fixtureDir, "devbox-exact.bin");
  try {
    const devboxWrite = await client.callTool({
      name: "devbox_write_file",
      arguments: { path: devboxTextPath, content: "alpha\nbeta\n" },
    });
    assert.equal(devboxWrite.isError, false);
    assert.equal(devboxWrite.structuredContent?.ok, true);

    const devboxRead = await client.callTool({
      name: "devbox_read_file",
      arguments: { path: devboxTextPath, max_bytes: 64 },
    });
    assert.equal(devboxRead.isError, false);
    assert.equal(devboxRead.structuredContent?.stdout, "alpha\nbeta\n");

    const devboxList = await client.callTool({
      name: "devbox_list_files",
      arguments: { path: fixtureDir, recursive: false, max_entries: 20 },
    });
    assert.equal(devboxList.isError, false);
    assert.match(devboxList.structuredContent?.stdout || "", /devbox-text\.txt/);

    const devboxLargeWrite = await client.callTool({
      name: "devbox_write_large_file",
      arguments: {
        path: devboxExactPath,
        content_base64: "AP9hbHBoYQo=",
        expected_sha256: "5cd13e70af539c9471799a5cf52bc04af6ee4a13bd866523861a1fc51fa6acb5",
      },
    });
    assert.equal(devboxLargeWrite.isError, false);
    assert.equal(devboxLargeWrite.structuredContent?.data?.verified, true);

    const devboxLargeRead = await client.callTool({
      name: "devbox_read_large_file",
      arguments: { path: devboxExactPath, offset_bytes: 1, max_bytes: 4 },
    });
    assert.equal(devboxLargeRead.isError, false);
    assert.equal(devboxLargeRead.structuredContent?.data?.content_base64, "/2FscA==");
    assert.ok(!devboxLargeRead.content?.[0]?.text?.includes("/2FscA=="), "devbox large read must keep raw base64 out of text content");

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
