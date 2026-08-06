import assert from "node:assert/strict";
import { createHash, randomUUID } from "node:crypto";
import path from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const args = process.argv.slice(2);
const option = (name, fallback = "") => {
  const index = args.indexOf(name);
  return index >= 0 && index + 1 < args.length ? args[index + 1] : fallback;
};

const url = option("--url", process.env.DEVBOX_E2E_URL || "http://127.0.0.1:18180/");
const workspace = path.resolve(option("--workspace", process.env.DEVBOX_E2E_WORKSPACE || process.cwd()));
const expectedPlatform = option("--expect-platform", process.env.DEVBOX_E2E_EXPECT_PLATFORM || "");
const marker = `DEVBOX_PLATFORM_E2E_${randomUUID().replaceAll("-", "")}`;
const testDir = path.join(workspace, ".platform-e2e");
const textFile = path.join(testDir, "bridge.txt");
const execFile = path.join(testDir, "exec.txt");
const largeFile = path.join(testDir, "large.bin");

const shellQuote = (value) => `'${String(value).replaceAll("'", `'"'"'`)}'`;
const requireSuccess = (name, result) => {
  assert.equal(
    result?.structuredContent?.ok,
    true,
    `${name} failed:\n${JSON.stringify(result, null, 2)}`,
  );
  return result.structuredContent;
};

const transport = new StreamableHTTPClientTransport(new URL(url));
const client = new Client({ name: "platform-runtime-e2e", version: "1.0.0" });

try {
  await client.connect(transport);

  const listed = await client.listTools();
  const toolNames = new Set((listed.tools || []).map((tool) => tool.name));
  const requiredTools = [
    "devbox_status",
    "devbox_exec_readonly",
    "devbox_exec",
    "devbox_list_files",
    "devbox_read_file",
    "devbox_write_file",
    "devbox_read_large_file",
    "devbox_write_large_file",
    "devbox_search_files",
    "host_status",
    "host_exec",
    "host_run_program",
  ];
  for (const name of requiredTools) {
    assert.ok(toolNames.has(name), `Expected MCP tool ${name} to be registered.`);
  }

  const status = requireSuccess("devbox_status", await client.callTool({ name: "devbox_status", arguments: {} }));
  assert.equal(status.data?.mode, "host");
  assert.equal(status.data?.status, "ready");
  if (expectedPlatform) {
    assert.equal(status.data?.platform, expectedPlatform, `Expected platform ${expectedPlatform}.`);
  }

  const hostStatus = requireSuccess("host_status", await client.callTool({ name: "host_status", arguments: {} }));
  assert.equal(hostStatus.data?.enabled, true, "Host execution must be enabled in runtime E2E.");
  if (expectedPlatform) {
    assert.equal(hostStatus.data?.platform, expectedPlatform);
  }

  const readonly = requireSuccess(
    "devbox_exec_readonly",
    await client.callTool({
      name: "devbox_exec_readonly",
      arguments: {
        command: "printf '%s' 'DEVBOX_E2E_READONLY_OK'",
        working_dir: workspace,
        timeout_seconds: 30,
      },
    }),
  );
  assert.match(readonly.stdout || "", /DEVBOX_E2E_READONLY_OK/u);

  requireSuccess(
    "devbox_write_file",
    await client.callTool({
      name: "devbox_write_file",
      arguments: { path: textFile, content: `${marker}\n`, append: false, create_dirs: true },
    }),
  );

  const readText = requireSuccess(
    "devbox_read_file",
    await client.callTool({ name: "devbox_read_file", arguments: { path: textFile, max_bytes: 65536 } }),
  );
  assert.match(readText.stdout || "", new RegExp(marker, "u"));

  const listedFiles = requireSuccess(
    "devbox_list_files",
    await client.callTool({
      name: "devbox_list_files",
      arguments: { path: testDir, recursive: true, max_depth: 3, max_entries: 100, timeout_seconds: 30 },
    }),
  );
  assert.match(listedFiles.stdout || "", /bridge\.txt/u);

  const searched = requireSuccess(
    "devbox_search_files",
    await client.callTool({
      name: "devbox_search_files",
      arguments: {
        pattern: marker,
        path: testDir,
        glob: "*",
        case_sensitive: true,
        max_matches: 20,
        max_depth: 3,
        max_file_bytes: 1048576,
        timeout_seconds: 30,
      },
    }),
  );
  assert.match(searched.stdout || "", new RegExp(marker, "u"));

  const largePayload = Buffer.from(`${marker}\n${"0123456789abcdef".repeat(8192)}\n`, "utf8");
  const largeSha = createHash("sha256").update(largePayload).digest("hex");
  const largeWrite = requireSuccess(
    "devbox_write_large_file",
    await client.callTool({
      name: "devbox_write_large_file",
      arguments: {
        path: largeFile,
        content_base64: largePayload.toString("base64"),
        append: false,
        create_dirs: true,
        expected_sha256: largeSha,
      },
    }),
  );
  assert.equal(largeWrite.data?.bytes_written, largePayload.length);

  const largeRead = requireSuccess(
    "devbox_read_large_file",
    await client.callTool({
      name: "devbox_read_large_file",
      arguments: { path: largeFile, offset_bytes: 0, max_bytes: largePayload.length },
    }),
  );
  assert.equal(largeRead.data?.content_sha256, largeSha);
  assert.deepEqual(Buffer.from(largeRead.data?.content_base64 || "", "base64"), largePayload);

  requireSuccess(
    "devbox_exec",
    await client.callTool({
      name: "devbox_exec",
      arguments: {
        command: `mkdir -p ${shellQuote(testDir)} && printf '%s' '${marker}_EXEC' > ${shellQuote(execFile)}`,
        working_dir: workspace,
        timeout_seconds: 30,
      },
    }),
  );
  const execRead = requireSuccess(
    "devbox_read_file after devbox_exec",
    await client.callTool({ name: "devbox_read_file", arguments: { path: execFile, max_bytes: 65536 } }),
  );
  assert.match(execRead.stdout || "", new RegExp(`${marker}_EXEC`, "u"));

  const hostExec = requireSuccess(
    "host_exec",
    await client.callTool({
      name: "host_exec",
      arguments: {
        command: "printf '%s' 'DEVBOX_E2E_HOST_OK'",
        working_dir: workspace,
        timeout_seconds: 30,
      },
    }),
  );
  assert.match(hostExec.stdout || "", /DEVBOX_E2E_HOST_OK/u);

  const hostProgram = requireSuccess(
    "host_run_program",
    await client.callTool({
      name: "host_run_program",
      arguments: { program: "node", args: ["--version"], working_dir: workspace, timeout_seconds: 30 },
    }),
  );
  assert.match(hostProgram.stdout || "", /^v\d+/u);

  console.log(JSON.stringify({
    ok: true,
    url,
    workspace,
    platform: status.data?.platform,
    shell: hostStatus.data?.shell,
    toolsVerified: requiredTools.length,
    marker,
  }, null, 2));
} finally {
  await client.close().catch(() => {});
}
