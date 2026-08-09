import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const baseUrl = new URL(process.env.RUST_MCP_URL || "http://127.0.0.1:18182/");
const expectedWindowsAdmin = process.platform === "win32"
  ? process.env.RUST_MCP_EXPECT_WINDOWS_ADMIN === "1"
  : null;

const assertShellResult = (result, { readOnly } = {}) => {
  if (process.platform === "win32" && !expectedWindowsAdmin) {
    assert.equal(result.isError, true);
    assert.equal(result.structuredContent?.exitCode, 740);
    assert.equal(result.structuredContent?.data?.bridge_diagnostics?.suspected_elevation_gap, true);
    assert.equal(result.structuredContent?.data?.bridge_diagnostics?.windows_host_exec_defaults_to_admin, true);
    assert.equal(result.structuredContent?.data?.bridge_diagnostics?.allow_windows_host_exec_uac, false);
    assert.equal(result.structuredContent?.data?.bridge_diagnostics?.hints?.length, 3);
    return;
  }
  assert.equal(result.isError, false);
  assert.match(result.structuredContent?.stdout || "", /^git version /);
  if (readOnly !== undefined) assert.equal(result.structuredContent?.data?.read_only, readOnly);
  assert.equal(result.structuredContent?.data?.execution?.pool, "execution");
};


const healthResponse = await fetch(new URL("healthz", baseUrl));
assert.equal(healthResponse.status, 200);
assert.equal(await healthResponse.text(), "ok");

const metadataResponse = await fetch(baseUrl);
assert.equal(metadataResponse.status, 200);
const metadata = await metadataResponse.json();

for (const probeUrl of [baseUrl, new URL("mcp", baseUrl)]) {
  const rejectedProbe = await fetch(probeUrl, { headers: { Accept: "application/json" } });
  if (probeUrl.pathname.endsWith("/mcp")) {
    assert.equal(rejectedProbe.status, 406);
    assert.match((await rejectedProbe.json()).error?.message || "", /text\/event-stream/);
  } else {
    assert.equal(rejectedProbe.status, 200);
  }

  const sseProbe = await fetch(probeUrl, { headers: { Accept: "text/event-stream" } });
  assert.equal(sseProbe.status, 200);
  assert.match(sseProbe.headers.get("content-type") || "", /^text\/event-stream/);
  assert.equal(sseProbe.headers.get("cache-control"), "no-cache, no-transform");
  assert.match(await sseProbe.text(), /mcp-sse-probe/);
}

for (const endpoint of [baseUrl, new URL("mcp", baseUrl)]) {
  const deletion = await fetch(endpoint, {
    method: "DELETE",
    headers: { Accept: "application/json, text/event-stream" },
  });
  assert.equal(deletion.status, 200, `${endpoint.pathname} should match the stateless JS DELETE response`);
  assert.equal(await deletion.text(), "");
}
assert.equal(metadata.auth_mode, "none");
assert.equal(metadata.runtime_mode, "host");
assert.equal(metadata.local_base_url, baseUrl.toString().replace(/\/$/, ""));
assert.equal(metadata.root_mcp_url, baseUrl.toString().replace(/\/$/, ""));
assert.equal(metadata.mcp_url, new URL("mcp", baseUrl).toString().replace(/\/$/, ""));
assert.equal(metadata.gateway_bridge?.enabled, true);
assert.equal(metadata.gateway_bridge?.private_network_access, true);
assert.deepEqual(metadata.gateway_bridge?.origins, ["https://chatgpt.com", "https://chat.openai.com"]);
assert.equal(metadata.runtime?.runtimeMode, "host");
assert.equal(metadata.devbox?.mode, "host");
const expectedTools = [
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
  "devbox_search_files",
  "host_exec",
  "windows_host_exec",
  "host_run_program",
  "windows_host_run_program",
  "windows_host_inspect_file",
  "devbox_start",
  "devbox_stop",
  "devbox_restart",
  "devbox_recreate",
  "devbox_github_auth_status",
  "devbox_sync_github_auth_from_host",
  "host_capture_display",
  "host_capture_window",
  "host_capture_program",
  "windows_host_capture_display",
  "windows_host_capture_program",
].sort();
assert.equal(expectedTools.length, 37);

const transport = new StreamableHTTPClientTransport(baseUrl);
const client = new Client({ name: "rust-parity-smoke", version: "0.1.0" });
try {
  await client.connect(transport);
  assert.deepEqual(client.getServerCapabilities()?.logging, {});
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

  const cancellationRequestId = client._requestMessageId;
  const cancelStarted = Date.now();
  const cancellableWait = client.callTool({
    name: "devbox_wait",
    arguments: { seconds: 10, reason: "rust-cancellation-smoke" },
  });
  setTimeout(() => {
    void client.notification({
      method: "notifications/cancelled",
      params: { requestId: cancellationRequestId, reason: "rust-cancellation-smoke" },
    });
  }, 150);
  const cancelledWait = await cancellableWait;
  assert.equal(cancelledWait.isError, true);
  assert.match(cancelledWait.content?.[0]?.text || "", /Wait was cancelled/i);
  assert.ok(Date.now() - cancelStarted < 3_000, "cancellation notification should stop the Rust handler promptly");

  const processCancellationRequestId = client._requestMessageId;
  const processCancelStarted = Date.now();
  const cancellableProcess = client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "node",
      args: ["-e", "setTimeout(() => {}, 10000)"],
      timeout_seconds: 20,
      max_output_chars: 2_000,
    },
  });
  setTimeout(() => {
    void client.notification({
      method: "notifications/cancelled",
      params: { requestId: processCancellationRequestId, reason: "rust-process-cancellation-smoke" },
    });
  }, 250);
  const cancelledProcess = await cancellableProcess;
  assert.equal(cancelledProcess.isError, true);
  assert.match(
    `${cancelledProcess.structuredContent?.summary || ""} ${cancelledProcess.structuredContent?.stderr || ""}`,
    /cancel|abort/i,
  );
  assert.ok(Date.now() - processCancelStarted < 4_000, "cancelled child process should terminate promptly");

  const status = await client.callTool({ name: "devbox_status", arguments: {} });
  assert.equal(status.isError, false);
  assert.equal(status.structuredContent?.ok, true);
  const statusData = status.structuredContent?.data || {};
  assert.equal(statusData.mode, "host");
  assert.equal(statusData.running, true);
  assert.equal(statusData.execution?.max_concurrent > 0, true);
  assert.equal(statusData.execution?.local_process?.acquired >= 0, true);
  assert.ok(Object.prototype.hasOwnProperty.call(statusData, "guardian"));
  assert.ok(Object.prototype.hasOwnProperty.call(statusData, "startup"));
  assert.equal(statusData.performance?.process?.pid > 0, true);
  assert.equal(statusData.performance?.process?.uptimeSeconds >= 0, true);
  assert.equal(statusData.performance?.process?.memory?.rss >= 0, true);
  assert.equal(statusData.performance?.eventLoop?.p95Ms >= 0, true);
  assert.match(statusData.performance?.eventLoop?.sampledAtUtc || "", /^\d{4}-\d{2}-\d{2}T/);
  assert.ok(Array.isArray(statusData.versions));
  for (const program of process.platform === "win32" ? ["node", "npm", "git", "gh", "python"] : ["node", "npm", "git", "gh", "python3", "rg"]) {
    assert.ok(statusData.versions.some((line) => line.startsWith(`${program}=`)), `missing version status for ${program}`);
  }

  for (const toolName of ["host_status", "windows_host_status"]) {
    const hostStatus = await client.callTool({ name: toolName, arguments: {} });
    assert.equal(hostStatus.isError, false);
    const hostData = hostStatus.structuredContent?.data || {};
    assert.equal(hostData.enabled, true);
    assert.equal(hostData.platform, metadata.platform);
    assert.ok(Array.isArray(hostData.allowlist));
    assert.ok((hostData.resolvedNodeExe || "").length > 0);
    assert.equal(typeof hostData.powerShellFallbackEnabled, "boolean");
    assert.equal(hostData.windowsHostExecDefaultsToAdmin, process.platform === "win32");
    assert.equal(typeof hostData.allowWindowsHostExecUac, "boolean");
  }

  if (process.env.RUST_MCP_SMOKE_GITHUB_AUTH === "1") {
    const githubAuth = await client.callTool({
      name: "devbox_github_auth_status",
      arguments: {},
    });
    assert.equal(githubAuth.isError, false);
    assert.equal(githubAuth.structuredContent?.ok, true);
    const authData = githubAuth.structuredContent?.data || {};
    assert.ok((authData.statusSummary || "").length > 0);
    assert.equal(Object.prototype.hasOwnProperty.call(authData, "token"), false);
    assert.ok(!JSON.stringify(githubAuth.structuredContent).includes('"token"'));
  }

  const oversizedCapturePid = await client.callTool({
    name: "host_capture_window",
    arguments: { pid: Number.MAX_SAFE_INTEGER, quality: 80, include_process_tree: false },
  });
  assert.equal(oversizedCapturePid.isError, true);
  assert.match(
    oversizedCapturePid.content?.find((entry) => entry.type === "text")?.text || "",
    /pid exceeds .*process ID range/i,
  );

  if (process.env.RUST_MCP_SMOKE_CAPTURE_PID) {
    const capturePid = Number.parseInt(process.env.RUST_MCP_SMOKE_CAPTURE_PID, 10);
    assert.ok(Number.isInteger(capturePid) && capturePid > 0, "RUST_MCP_SMOKE_CAPTURE_PID must be a positive integer");

    const assertCapture = (capture, { expectedPid } = {}) => {
      assert.equal(capture.isError, false);
      assert.equal(capture.structuredContent?.ok, true);
      const image = capture.content?.find((entry) => entry.type === "image");
      assert.ok(image?.data?.length > 100, "capture should contain non-empty base64 image data");
      assert.match(image?.mimeType || "", /^image\/(?:jpeg|png)$/);
      const data = capture.structuredContent?.data || {};
      assert.ok((data.bytes || 0) > 0);
      assert.match(data.sha256 || "", /^[0-9a-f]{64}$/);
      assert.ok((data.capture_attempts || 0) >= 1);
      assert.ok((data.width || 0) > 0);
      assert.ok((data.height || 0) > 0);
      if (expectedPid !== undefined) assert.equal(data.pid, expectedPid);
    };

    for (const toolName of ["host_capture_display", "windows_host_capture_display"]) {
      const capture = await client.callTool({ name: toolName, arguments: { quality: 80 } });
      assertCapture(capture);
    }

    for (const toolName of ["host_capture_window", "host_capture_program", "windows_host_capture_program"]) {
      const capture = await client.callTool({
        name: toolName,
        arguments: { pid: capturePid, quality: 80, include_process_tree: true },
      });
      assertCapture(capture, { expectedPid: capturePid });
    }
  }

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
    assertShellResult(shell, { readOnly });
  }

  for (const toolName of ["host_exec", "windows_host_exec"]) {
    const hostShell = await client.callTool({
      name: toolName,
      arguments: { command: "git --version", max_output_chars: 2_000 },
    });
    assertShellResult(hostShell);
  }

  if (process.platform === "win32" && expectedWindowsAdmin) {
    const largePowerShell = await client.callTool({
      name: "host_exec",
      arguments: {
        command: `${"#"}${"x".repeat(20_000)}\nWrite-Output 'large-powershell-ok'`,
        max_output_chars: 2_000,
      },
    });
    assert.equal(largePowerShell.isError, false);
    assert.match(largePowerShell.structuredContent?.stdout || "", /large-powershell-ok/);
  }

  for (const toolName of ["host_run_program", "windows_host_run_program"]) {
    const hostProgram = await client.callTool({
      name: toolName,
      arguments: { program: "git", args: ["--version"], max_output_chars: 2_000 },
    });
    assert.equal(hostProgram.isError, false);
    assert.match(hostProgram.structuredContent?.stdout || "", /^git version /);
    assert.equal(hostProgram.structuredContent?.data?.execution?.pool, "execution");
  }

  const stateRoot = process.env.RUST_MCP_STATE_ROOT;
  const readGuardianDesired = async () => {
    assert.ok(stateRoot, "RUST_MCP_STATE_ROOT is required for lifecycle smoke assertions");
    return JSON.parse(await readFile(path.join(stateRoot, "run", "guardian.desired-state.json"), "utf8"));
  };

  const startedRuntime = await client.callTool({ name: "devbox_start", arguments: {} });
  assert.equal(startedRuntime.isError, false);
  assert.equal(startedRuntime.structuredContent?.data?.mode, "host");
  assert.equal(startedRuntime.structuredContent?.data?.running, true);
  const startDesired = await readGuardianDesired();
  assert.equal(startDesired.ShouldRun, true);
  assert.equal(startDesired.Source, "src/server.js:devbox_start");
  assert.match(startDesired.UpdatedAtUtc || "", /^\d{4}-\d{2}-\d{2}T/);

  for (const [toolName, action, shouldRun] of [
    ["devbox_stop", "stop", false],
    ["devbox_restart", "restart", true],
    ["devbox_recreate", "recreate", true],
  ]) {
    const lifecycle = await client.callTool({ name: toolName, arguments: {} });
    assert.equal(lifecycle.isError, false);
    assert.equal(lifecycle.structuredContent?.data?.controlAction, action);
    assert.match(lifecycle.structuredContent?.data?.controlMessage || "", /launcher command/);
    assert.equal(lifecycle.structuredContent?.data?.running, true);
    const desired = await readGuardianDesired();
    assert.equal(desired.ShouldRun, shouldRun);
    assert.equal(desired.Source, `src/server.js:devbox_${action}`);
  }

  const shellStarted = await client.callTool({
    name: "devbox_exec_start",
    arguments: {
      command: "node -e \"console.log('RUST_SHELL_ASYNC_SMOKE')\"",
      timeout_seconds: 30,
      read_only: true,
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
  assert.equal(shellDone.structuredContent?.data?.readOnly, true);
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

    const inspected = await client.callTool({
      name: "windows_host_inspect_file",
      arguments: { path: devboxTextPath, max_bytes: 1_024 },
    });
    assert.equal(inspected.isError, false);
    assert.equal(inspected.structuredContent?.data?.exists, true);
    assert.equal(inspected.structuredContent?.data?.is_file, true);
    assert.equal(inspected.structuredContent?.data?.utf8_valid, true);
    assert.equal(inspected.structuredContent?.data?.line_endings, "lf");
    assert.equal(inspected.structuredContent?.data?.likely_corrupted_on_disk, false);
    assert.match(inspected.structuredContent?.data?.preview || "", /^alpha\nbeta/);

    if (process.platform === "win32") {
      const validPs1 = path.join(fixtureDir, "valid.ps1");
      const invalidPs1 = path.join(fixtureDir, "invalid.ps1");
      await writeFile(validPs1, "Write-Output 'ok'\n", "utf8");
      await writeFile(invalidPs1, "function Broken {\n", "utf8");

      const validPowerShell = await client.callTool({
        name: "windows_host_inspect_file",
        arguments: { path: validPs1, max_bytes: 4_096 },
      });
      assert.equal(validPowerShell.isError, false);
      assert.equal(validPowerShell.structuredContent?.data?.powershell_syntax?.parse_ok, true);
      assert.equal(validPowerShell.structuredContent?.data?.syntax_invalid, false);

      const invalidPowerShell = await client.callTool({
        name: "windows_host_inspect_file",
        arguments: { path: invalidPs1, max_bytes: 4_096 },
      });
      assert.equal(invalidPowerShell.isError, false);
      assert.equal(invalidPowerShell.structuredContent?.data?.powershell_syntax?.parse_ok, false);
      assert.ok((invalidPowerShell.structuredContent?.data?.powershell_syntax?.error_count || 0) > 0);
      assert.equal(invalidPowerShell.structuredContent?.data?.syntax_invalid, true);
    }

    const devboxList = await client.callTool({
      name: "devbox_list_files",
      arguments: { path: fixtureDir, recursive: false, max_entries: 20 },
    });
    assert.equal(devboxList.isError, false);
    assert.match(devboxList.structuredContent?.stdout || "", /devbox-text\.txt/);

    const devboxSearch = await client.callTool({
      name: "devbox_search_files",
      arguments: {
        pattern: "beta",
        path: fixtureDir,
        glob: "*.txt",
        max_matches: 10,
        max_depth: 4,
      },
    });
    assert.equal(devboxSearch.isError, false);
    assert.match(devboxSearch.structuredContent?.stdout || "", /devbox-text\.txt:2:beta/);
    assert.match(devboxSearch.structuredContent?.stderr || "", /search backend (?:ripgrep|rust fallback)/);
    assert.equal(devboxSearch.structuredContent?.data?.execution?.pool, "execution");

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

  assert.ok(stateRoot, "RUST_MCP_STATE_ROOT is required for usage telemetry assertions");
  const toolUsageText = await readFile(path.join(stateRoot, "run", "tool-usage.jsonl"), "utf8");
  assert.ok(!toolUsageText.includes("AP9hbHBoYQo="), "sensitive content_base64 must be redacted from tool telemetry");
  const toolUsage = toolUsageText.trim().split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
  const waitStart = toolUsage.find((event) => event.type === "tool_start" && event.tool === "devbox_wait");
  const waitFinish = toolUsage.find((event) => event.type === "tool_finish" && event.tool === "devbox_wait");
  assert.ok(waitStart?.invocation_id);
  assert.equal(waitFinish?.invocation_id, waitStart.invocation_id);
  assert.equal(waitStart.arguments?.reason?.preview, "rust-shadow-smoke");
  assert.ok(waitStart.context?.request_id !== undefined);
  assert.equal(waitFinish.ok, true);
  assert.equal(waitFinish.is_error, false);
  assert.ok(waitFinish.duration_ms >= 0);
  const cancelStart = toolUsage.find((event) => event.type === "tool_start" && event.tool === "devbox_wait" && event.arguments?.reason?.preview === "rust-cancellation-smoke");
  const cancelFinish = toolUsage.find((event) => event.type === "tool_finish" && event.invocation_id === cancelStart?.invocation_id);
  assert.ok(cancelStart?.invocation_id, "cancelled wait must reach the Rust tool handler");
  assert.ok(cancelFinish, "cancellation notification must finish the Rust handler");
  assert.equal(cancelFinish.is_error, true);
  assert.ok(cancelFinish.duration_ms < 3_000);
  const processCancelCandidates = toolUsage.filter((event) => event.type === "tool_start" && event.tool === "devbox_run_program");
  const processCancelEvent = processCancelCandidates.find((event) =>
    JSON.stringify(event.arguments || {}).includes("setTimeout(() => {}, 10000)"),
  );
  const processCancelFinish = toolUsage.find((event) => event.type === "tool_finish" && event.invocation_id === processCancelEvent?.invocation_id);
  assert.ok(processCancelEvent?.invocation_id, "cancelled process must reach Rust tool handler");
  assert.ok(processCancelFinish, "cancelled process must produce a finish telemetry event");
  assert.equal(processCancelFinish.is_error, true);
  assert.ok(processCancelFinish.duration_ms < 4_000);

  const httpUsageText = await readFile(path.join(stateRoot, "run", "http-usage.jsonl"), "utf8");
  const httpUsage = httpUsageText.trim().split(/\r?\n/).filter(Boolean).map((line) => JSON.parse(line));
  const rootGet = httpUsage.find((event) => event.type === "http_request" && event.method === "GET" && event.path === "/" && event.outcome === "finished");
  const mcpPost = httpUsage.find((event) => event.type === "http_request" && event.method === "POST" && event.path === "/" && event.outcome === "finished");
  assert.equal(rootGet?.status_code, 200);
  assert.ok(rootGet?.request_id);
  assert.equal(rootGet?.client_aborted, false);
  assert.ok(mcpPost?.status_code >= 200 && mcpPost?.status_code < 300);
  assert.match(mcpPost?.user_agent || "", /modelcontextprotocol|node|undici/i);

  console.log(JSON.stringify({
    ok: true,
    baseUrl: baseUrl.toString(),
    tools: names,
    toolCount: names.length,
    waitMs: wait.structuredContent.data.waited_ms,
    cancellationHandlerFinished: true,
  }, null, 2));
} finally {
  await client.close();
}
