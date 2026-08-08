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

const waitForHealth = async (baseUrl, timeoutMs = 20000) => {
  const healthUrl = new URL("/healthz", baseUrl).toString();
  const deadline = Date.now() + timeoutMs;
  let lastError = null;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(healthUrl);
      if (response.ok) return healthUrl;
      lastError = new Error(`HTTP ${response.status}`);
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Runtime did not become healthy at ${healthUrl}: ${lastError instanceof Error ? lastError.message : String(lastError)}`);
};

const transport = new StreamableHTTPClientTransport(new URL(url));
const client = new Client({ name: "platform-runtime-e2e", version: "1.0.0" });

try {
  await waitForHealth(url);
  await client.connect(transport);

  const listed = await client.listTools();
  const toolNames = new Set((listed.tools || []).map((tool) => tool.name));
  const requiredTools = [
    "devbox_status",
    "devbox_exec_readonly",
    "devbox_exec",
    "devbox_run_program",
    "devbox_run_program_start",
    "devbox_exec_start",
    "devbox_job_status",
    "devbox_wait",
    "devbox_wait_for_file",
    "devbox_job_logs",
    "devbox_job_cancel",
    "devbox_list_files",
    "devbox_read_file",
    "devbox_write_file",
    "devbox_read_large_file",
    "devbox_write_large_file",
    "devbox_search_files",
    "host_status",
    "host_capture_display",
    "host_capture_window",
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
  const isWindowsHost = status.data?.platform === "windows";
  const psLiteral = (value) => `'${String(value).replaceAll("'", "''")}'`;
  const readonlyShellCommand = isWindowsHost
    ? "[Console]::Out.Write('DEVBOX_E2E_READONLY_OK')"
    : "printf '%s' 'DEVBOX_E2E_READONLY_OK'";
  const mutatingShellCommand = isWindowsHost
    ? `New-Item -ItemType Directory -Force -Path ${psLiteral(testDir)} | Out-Null; Set-Content -LiteralPath ${psLiteral(execFile)} -Value ${psLiteral(`${marker}_EXEC`)} -NoNewline`
    : `mkdir -p ${shellQuote(testDir)} && printf '%s' '${marker}_EXEC' > ${shellQuote(execFile)}`;
  const asyncShellCommand = isWindowsHost
    ? "Write-Output 'ASYNC_JOB_START'; Start-Sleep -Seconds 1; Write-Output 'ASYNC_JOB_DONE'"
    : "printf 'ASYNC_JOB_START\n'; sleep 1; printf 'ASYNC_JOB_DONE\n'";
  const cancelShellCommand = isWindowsHost
    ? "Write-Output 'ASYNC_CANCEL_START'; Start-Sleep -Seconds 30"
    : "printf 'ASYNC_CANCEL_START\n'; sleep 30";
  const hostShellCommand = isWindowsHost
    ? "[Console]::Out.Write('DEVBOX_E2E_HOST_OK')"
    : "printf '%s' 'DEVBOX_E2E_HOST_OK'";

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
        command: readonlyShellCommand,
        working_dir: workspace,
        timeout_seconds: 30,
      },
    }),
  );
  assert.match(readonly.stdout || "", /DEVBOX_E2E_READONLY_OK/u);

  const directProgram = requireSuccess(
    "devbox_run_program",
    await client.callTool({
      name: "devbox_run_program",
      arguments: {
        program: "node",
        args: ["-e", "process.stdout.write('DEVBOX_E2E_DIRECT_OK')"],
        working_dir: workspace,
        timeout_seconds: 30,
      },
    }),
  );
  assert.match(directProgram.stdout || "", /DEVBOX_E2E_DIRECT_OK/u);

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
        command: mutatingShellCommand,
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


  const backgroundStart = requireSuccess(
    "devbox_exec_start",
    await client.callTool({
      name: "devbox_exec_start",
      arguments: {
        command: asyncShellCommand,
        working_dir: workspace,
        timeout_seconds: 30,
        read_only: true,
      },
    }),
  );
  const backgroundJobId = backgroundStart.data?.id;
  assert.match(backgroundJobId || "", /^job-/u);
  let backgroundStatus = null;
  for (let attempt = 0; attempt < 100; attempt += 1) {
    backgroundStatus = requireSuccess(
      "devbox_job_status",
      await client.callTool({ name: "devbox_job_status", arguments: { job_id: backgroundJobId } }),
    );
    if (["succeeded", "failed", "cancelled", "timed_out"].includes(backgroundStatus.data?.status)) break;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  assert.equal(backgroundStatus?.data?.status, "succeeded");
  const backgroundLogs = requireSuccess(
    "devbox_job_logs",
    await client.callTool({ name: "devbox_job_logs", arguments: { job_id: backgroundJobId, max_chars: 5000 } }),
  );
  assert.match(backgroundLogs.data?.stdout || "", /ASYNC_JOB_START/u);
  assert.match(backgroundLogs.data?.stdout || "", /ASYNC_JOB_DONE/u);

  const cancelStart = requireSuccess(
    "devbox_exec_start cancel fixture",
    await client.callTool({
      name: "devbox_exec_start",
      arguments: {
        command: cancelShellCommand,
        working_dir: workspace,
        timeout_seconds: 60,
        read_only: true,
      },
    }),
  );
  const cancelJobId = cancelStart.data?.id;
  for (let attempt = 0; attempt < 50; attempt += 1) {
    const pending = requireSuccess(
      "devbox_job_status cancel fixture",
      await client.callTool({ name: "devbox_job_status", arguments: { job_id: cancelJobId } }),
    );
    if (pending.data?.status === "running") break;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  const cancelled = requireSuccess(
    "devbox_job_cancel",
    await client.callTool({ name: "devbox_job_cancel", arguments: { job_id: cancelJobId } }),
  );
  assert.equal(cancelled.data?.status, "cancelled");

  const hostExec = requireSuccess(
    "host_exec",
    await client.callTool({
      name: "host_exec",
      arguments: {
        command: hostShellCommand,
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
