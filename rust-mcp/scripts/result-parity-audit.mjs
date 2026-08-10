import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDir, "..", "..");
const serverPath = path.join(projectRoot, "src", "server.js");
const binaryPath = path.join(projectRoot, "rust-mcp", "target", "debug", process.platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const fixtureRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-result-fixture-"));
const jsStateRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-result-js-state-"));
const rustStateRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-result-rust-state-"));
const ghConfigRoot = await mkdtemp(path.join(os.tmpdir(), "devbox-result-gh-config-"));
await mkdir(path.join(fixtureRoot, "nested"), { recursive: true });
await writeFile(path.join(fixtureRoot, "alpha.txt"), "alpha\nbeta\n", "utf8");
await writeFile(path.join(fixtureRoot, "nested", "bravo.txt"), "needle Alpha\nsecond line\n", "utf8");
await writeFile(path.join(fixtureRoot, "bytes.bin"), Buffer.from([0, 1, 2, 3, 250, 251, 252, 253]));
await writeFile(path.join(fixtureRoot, "large-write.bin"), Buffer.from("12345"));
await writeFile(path.join(fixtureRoot, "host-large-write.bin"), Buffer.from("12345"));

const commonEnv = {
  MCP_AUTH_MODE: "none",
  PUBLIC_BASE_URL: "",
  DEVBOX_RUNTIME_MODE: "host",
  ENABLE_HOST_EXEC: "true",
  HOST_SEARCH_BACKEND: "js",
  MAX_TEXT_OUTPUT_CHARS: "4000000",
  MAX_COMMAND_OUTPUT_CHARS: "65536",
  MAX_MCP_TRANSFER_CHARS: "4000000",
  HOST_WORKSPACE_PATH: fixtureRoot,
  HOST_DEFAULT_WORKDIR: fixtureRoot,
  DEVBOX_WORKSPACE_PATH: fixtureRoot,
  MCP_JOBS_ROOT: path.join(jsStateRoot, "jobs"),
  MCP_EXEC_SLOT_ROOT: path.join(jsStateRoot, "slots"),
  GH_CONFIG_DIR: ghConfigRoot,
};
Object.assign(process.env, commonEnv);

const reservePort = () => new Promise((resolve, reject) => {
  const server = net.createServer();
  server.unref();
  server.once("error", reject);
  server.listen(0, "127.0.0.1", () => {
    const address = server.address();
    assert.ok(address && typeof address === "object");
    server.close((error) => error ? reject(error) : resolve(address.port));
  });
});

const startJs = async () => {
  const source = (await readFile(serverPath, "utf8"))
    .replace(/export \{ app \};/, "export { app, buildServer };")
    .replace(
      /const projectRoot = path\.resolve\(path\.dirname\(fileURLToPath\(import\.meta\.url\)\), "\.\."\);/,
      `const projectRoot = ${JSON.stringify(jsStateRoot)};`,
    );
  assert.match(source, /export \{ app, buildServer \};/);
  assert.ok(source.includes(`const projectRoot = ${JSON.stringify(jsStateRoot)};`));
  const instrumentedPath = path.join(path.dirname(serverPath), `.result-parity-server-${process.pid}-${Date.now()}.mjs`);
  await writeFile(instrumentedPath, source, "utf8");
  const module = await import(`${pathToFileURL(instrumentedPath).href}?audit=${Date.now()}`);
  const server = module.buildServer();
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  const client = new Client({ name: "js-result-parity", version: "1.0.0" });
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  return { client, close: async () => { await client.close(); await server.close(); await rm(instrumentedPath, { force: true }); } };
};

const startRust = async () => {
  const port = await reservePort();
  let stderr = "";
  const server = spawn(binaryPath, [], {
    cwd: projectRoot,
    env: {
      ...process.env,
      ...commonEnv,
      DEVBOX_PROJECT_ROOT: rustStateRoot,
      MCP_JOBS_ROOT: path.join(rustStateRoot, "jobs"),
      MCP_EXEC_SLOT_ROOT: path.join(rustStateRoot, "slots"),
      HOST: "127.0.0.1",
      PORT: String(port),
    },
    stdio: ["ignore", "ignore", "pipe"],
    windowsHide: true,
  });
  server.stderr.setEncoding("utf8");
  server.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-12000); });
  const exited = new Promise((resolve, reject) => {
    server.once("error", reject);
    server.once("exit", (code, signal) => resolve({ code, signal }));
  });
  const baseUrl = new URL(`http://127.0.0.1:${port}/`);
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const early = await Promise.race([exited.then((result) => ({ result })), sleep(100).then(() => null)]);
    if (early) throw new Error(`Rust server exited ${JSON.stringify(early.result)}\n${stderr}`);
    try { if ((await fetch(new URL("healthz", baseUrl))).ok) break; } catch {}
  }
  const client = new Client({ name: "rust-result-parity", version: "1.0.0" });
  await client.connect(new StreamableHTTPClientTransport(baseUrl));
  return { client, close: async () => { await client.close(); if (server.exitCode === null && server.signalCode === null) server.kill(); await Promise.race([exited, sleep(5000)]); } };
};

const volatileTimingKeys = new Set([
  "waited_ms", "waitedMs", "queue_wait_ms", "elapsed_ms", "elapsedMs",
  "average_queue_wait_ms", "max_queue_wait_ms",
  "startedAtUtc", "completedAtUtc", "createdAtUtc", "updatedAtUtc", "UpdatedAtUtc", "sampledAtUtc",
  "queuedAtUtc", "heartbeatAgeMs", "queueWaitMs", "capture_queue_wait_ms",
]);

const normalizeJobIds = (value) => typeof value === "string"
  ? value
      .replaceAll(jsStateRoot, "<state>")
      .replaceAll(rustStateRoot, "<state>")
      .replace(/job-[A-Za-z0-9_-]+/g, "<job>")
      .replace(/(?:[/\\][^/\\]+)*[/\\]devbox-macos-window-capture-[^/\\]+[/\\]devbox-window-query\.swift/g, "<capture>/devbox-window-query.swift")
  : value;

const stable = (value, pathParts = []) => {
  if (Array.isArray(value)) return value.map((item) => stable(item, pathParts));
  if (typeof value === "string") return normalizeJobIds(value);
  if (typeof value === "string") return normalizeJobIds(value);
  if (typeof value === "number" && pathParts.includes("performance")) return "<metric>";
  if (!value || typeof value !== "object") return value;
  const out = {};
  for (const key of Object.keys(value).sort()) {
    const item = value[key];
    if (["pid", "runnerPid", "childPid"].includes(key)) continue;
    if (volatileTimingKeys.has(key) && item !== null) {
      out[key] = "<timing>";
      continue;
    }
    out[key] = stable(item, [...pathParts, key]);
  }
  return out;
};

const normalizeTextValue = (value, pathParts = []) => {
  if (Array.isArray(value)) return value.map((item) => normalizeTextValue(item, pathParts));
  if (typeof value === "string") return normalizeJobIds(value);
  if (typeof value === "number" && pathParts.includes("performance")) return "<metric>";
  if (!value || typeof value !== "object") return value;
  const out = {};
  for (const key of Object.keys(value).sort()) {
    const item = value[key];
    if (["pid", "runnerPid", "childPid"].includes(key)) continue;
    if (volatileTimingKeys.has(key) && item !== null) {
      out[key] = "<timing>";
      continue;
    }
    out[key] = normalizeTextValue(item, [...pathParts, key]);
  }
  return out;
};


const calls = [
  ["devbox_read_file", { path: path.join(fixtureRoot, "alpha.txt"), max_bytes: 64 }],
  ["devbox_read_large_file", { path: path.join(fixtureRoot, "bytes.bin"), offset_bytes: 2, max_bytes: 4 }],
  ["devbox_list_files", { path: fixtureRoot, recursive: true, max_depth: 3, max_entries: 30, timeout_seconds: 10 }],
  ["devbox_search_files", { pattern: "needle", path: fixtureRoot, case_sensitive: false, max_matches: 20, max_depth: 4, timeout_seconds: 10 }],
  ["windows_host_inspect_file", { path: path.join(fixtureRoot, "alpha.txt"), working_dir: fixtureRoot, max_bytes: 64 }],
  ["devbox_wait", { seconds: 0.05, reason: "result-parity" }],
  ["host_status", {}],
  ["devbox_run_program", { program: "git", args: ["--version"], working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["devbox_exec_readonly", { command: "Write-Output 'RESULT_PARITY_READONLY'", working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["devbox_exec", { command: "Write-Output 'RESULT_PARITY_EXEC'", working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["host_exec", { command: "Write-Output 'RESULT_PARITY_HOST'", working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["windows_host_exec", { command: "Write-Output 'RESULT_PARITY_WINDOWS_ALIAS'", working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["host_run_program", { program: "git", args: ["--version"], working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["windows_host_run_program", { program: "git", args: ["--version"], working_dir: fixtureRoot, output_mode: "head", max_output_chars: 2000 }],
  ["windows_host_status", {}],
  ["devbox_wait_for_file", { path: path.join(fixtureRoot, "alpha.txt"), should_exist: true, min_bytes: 5, stable_ms: 0, timeout_seconds: 1, poll_ms: 50 }],
  ["devbox_write_file", { path: path.join(fixtureRoot, "written.txt"), content: "written\n", append: false, create_dirs: true }],
  ["devbox_write_large_file", { path: path.join(fixtureRoot, "large-write.bin"), content_base64: "SEVMTE8=", append: false, create_dirs: true }],
  ["windows_host_read_large_file", { path: path.join(fixtureRoot, "bytes.bin"), working_dir: fixtureRoot, offset_bytes: 1, max_bytes: 5 }],
  ["windows_host_write_large_file", { path: path.join(fixtureRoot, "host-large-write.bin"), working_dir: fixtureRoot, content_base64: "V09STEQ=", append: false, create_dirs: true }],
  ["devbox_read_file", { path: path.join(fixtureRoot, "missing.txt"), max_bytes: 64 }],
  ["devbox_run_program", { program: "definitely-not-allowlisted", args: [], working_dir: fixtureRoot, output_mode: "tail", max_output_chars: 2000 }],
  ["devbox_run_program", { program: "git", args: ["definitely-not-a-git-command"], working_dir: fixtureRoot, output_mode: "tail", max_output_chars: 4000 }],
  ["devbox_github_auth_status", {}],
  ["devbox_sync_github_auth_from_host", {}],
  ["host_capture_window", { pid: Number.MAX_SAFE_INTEGER, quality: 80, include_process_tree: false }],
  ["host_capture_program", { pid: Number.MAX_SAFE_INTEGER, quality: 80, include_process_tree: false }],
  ["windows_host_capture_program", { pid: Number.MAX_SAFE_INTEGER, quality: 80, include_process_tree: false }],
  ["devbox_status", {}],
  ["devbox_stop", {}],
  ["devbox_start", {}],
  ["devbox_restart", {}],
  ["devbox_recreate", {}],
];

const normalizeText = (name, result) => (result.content || []).map((entry) => {
  if (entry.type !== "text") return { type: entry.type };
  let text = normalizeJobIds(entry.text);
  const split = text.indexOf("\n\n");
  if (split >= 0) {
    const summary = text.slice(0, split);
    const raw = text.slice(split + 2);
    try {
      return { type: "text", text: `${summary}\n\n${JSON.stringify(normalizeTextValue(JSON.parse(raw)), null, 2)}` };
    } catch {}
  }
  text = text
    .replace(/"waited_ms": \d+/g, '"waited_ms": <timing>')
    .replace(/"waitedMs": \d+/g, '"waitedMs": <timing>')
    .replace(/"queue_wait_ms": \d+/g, '"queue_wait_ms": <timing>')
    .replace(/"queueWaitMs": \d+(?:\.\d+)?/g, '"queueWaitMs": <timing>')
    .replace(/"heartbeatAgeMs": \d+(?:\.\d+)?/g, '"heartbeatAgeMs": <timing>')
    .replace(/"elapsed_ms": \d+/g, '"elapsed_ms": <timing>');
  return { type: "text", text };
});

const normalizedResult = (name, result) => ({
  isError: result.isError ?? false,
  structuredContent: stable(result.structuredContent),
  content: normalizeText(name, result),
});

const normalizedCaptureResult = (result) => {
  const structured = structuredClone(result.structuredContent || {});
  if (structured.data && typeof structured.data === "object") {
    delete structured.data.bytes;
    delete structured.data.sha256;
    for (const key of [
      "print_window_mean_luma",
      "print_window_luma_range",
      "print_window_near_black_ratio",
      "print_window_interior_mean_luma",
      "print_window_interior_near_black_ratio",
      "candidate_pid_count",
    ]) {
      if (Object.hasOwn(structured.data, key)) structured.data[key] = "<capture-metric>";
    }
  }
  return {
    isError: result.isError ?? false,
    structuredContent: stable(structured),
    content: (result.content || []).map((entry) => entry.type === "image"
      ? { type: "image", mimeType: entry.mimeType }
      : { type: entry.type, summary: structured.summary }),
  };
};

const js = await startJs();
const rust = await startRust();
const differences = [];
let comparedCalls = 0;
const compareResults = (name, jsResult, rustResult) => {
  comparedCalls += 1;
  const a = normalizedResult(name, jsResult);
  const b = normalizedResult(name, rustResult);
  if (JSON.stringify(a) !== JSON.stringify(b)) differences.push({ name, js: a, rust: b });
};

const compareCaptureResults = (name, jsResult, rustResult) => {
  comparedCalls += 1;
  const a = normalizedCaptureResult(jsResult);
  const b = normalizedCaptureResult(rustResult);
  if (JSON.stringify(a) !== JSON.stringify(b)) differences.push({ name, js: a, rust: b });
};

try {
  for (const [name, args] of calls) {
    const legacyWindowsWriteTarget = name === "windows_host_write_large_file" && process.platform !== "win32"
      ? (path.win32.isAbsolute(args.path)
          ? path.win32.normalize(args.path)
          : path.win32.resolve(args.working_dir || fixtureRoot, args.path))
      : null;
    if (legacyWindowsWriteTarget) await rm(legacyWindowsWriteTarget, { force: true });
    const jsResult = await js.client.callTool({ name, arguments: args });
    if (legacyWindowsWriteTarget) await rm(legacyWindowsWriteTarget, { force: true });
    const rustResult = await rust.client.callTool({ name, arguments: args });
    if (legacyWindowsWriteTarget) await rm(legacyWindowsWriteTarget, { force: true });
    compareResults(name, jsResult, rustResult);
  }

  if (process.env.RUST_MCP_RESULT_SKIP_DISPLAY !== "1") {
    for (const name of ["host_capture_display", "windows_host_capture_display"]) {
      const jsResult = await js.client.callTool({ name, arguments: { quality: 80 } });
      const rustResult = await rust.client.callTool({ name, arguments: { quality: 80 } });
      compareCaptureResults(name, jsResult, rustResult);
    }
  }

  if (process.env.RUST_MCP_RESULT_CAPTURE_PID) {
    const capturePid = Number.parseInt(process.env.RUST_MCP_RESULT_CAPTURE_PID, 10);
    assert.ok(Number.isInteger(capturePid) && capturePid > 0);
    for (const name of ["host_capture_window", "host_capture_program", "windows_host_capture_program"]) {
      const args = { pid: capturePid, quality: 80, include_process_tree: true };
      const jsResult = await js.client.callTool({ name, arguments: args });
      const rustResult = await rust.client.callTool({ name, arguments: args });
      compareCaptureResults(name, jsResult, rustResult);
    }
  }

  const shellArgs = {
    command: "node -e \"console.log('RESULT_PARITY_JOB')\"",
    working_dir: fixtureRoot,
    timeout_seconds: 30,
    read_only: true,
    resource_class: "light",
  };
  const jsShellStart = await js.client.callTool({ name: "devbox_exec_start", arguments: shellArgs });
  const rustShellStart = await rust.client.callTool({ name: "devbox_exec_start", arguments: shellArgs });
  compareResults("devbox_exec_start", jsShellStart, rustShellStart);
  const jsShellId = jsShellStart.structuredContent?.data?.id;
  const rustShellId = rustShellStart.structuredContent?.data?.id;
  assert.ok(jsShellId && rustShellId);
  const jsShellDone = await js.client.callTool({ name: "devbox_job_status", arguments: { job_id: jsShellId, wait_seconds: 10, terminal_only: true } });
  const rustShellDone = await rust.client.callTool({ name: "devbox_job_status", arguments: { job_id: rustShellId, wait_seconds: 10, terminal_only: true } });
  compareResults("devbox_job_status", jsShellDone, rustShellDone);
  const jsShellLogs = await js.client.callTool({ name: "devbox_job_logs", arguments: { job_id: jsShellId, max_chars: 5000 } });
  const rustShellLogs = await rust.client.callTool({ name: "devbox_job_logs", arguments: { job_id: rustShellId, max_chars: 5000 } });
  compareResults("devbox_job_logs", jsShellLogs, rustShellLogs);

  const programArgs = {
    program: "node",
    args: ["-e", "setTimeout(()=>{},60000)"],
    working_dir: fixtureRoot,
    timeout_seconds: 60,
    resource_class: "light",
  };
  const jsProgramStart = await js.client.callTool({ name: "devbox_run_program_start", arguments: programArgs });
  const rustProgramStart = await rust.client.callTool({ name: "devbox_run_program_start", arguments: programArgs });
  compareResults("devbox_run_program_start", jsProgramStart, rustProgramStart);
  const jsProgramId = jsProgramStart.structuredContent?.data?.id;
  const rustProgramId = rustProgramStart.structuredContent?.data?.id;
  assert.ok(jsProgramId && rustProgramId);
  const waitUntilRunning = async (client, jobId) => {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const result = await client.callTool({ name: "devbox_job_status", arguments: { job_id: jobId, wait_seconds: 0, terminal_only: false } });
      if (result.structuredContent?.data?.status === "running") return result;
      await sleep(50);
    }
    throw new Error(`Job ${jobId} did not reach running state`);
  };
  const [jsRunning, rustRunning] = await Promise.all([
    waitUntilRunning(js.client, jsProgramId),
    waitUntilRunning(rust.client, rustProgramId),
  ]);
  compareResults("devbox_job_status", jsRunning, rustRunning);
  const [jsWaitTimeout, rustWaitTimeout] = await Promise.all([
    js.client.callTool({ name: "devbox_job_status", arguments: { job_id: jsProgramId, wait_seconds: 1, terminal_only: false } }),
    rust.client.callTool({ name: "devbox_job_status", arguments: { job_id: rustProgramId, wait_seconds: 1, terminal_only: false } }),
  ]);
  compareResults("devbox_job_status", jsWaitTimeout, rustWaitTimeout);
  const [jsCancel, rustCancel] = await Promise.all([
    js.client.callTool({ name: "devbox_job_cancel", arguments: { job_id: jsProgramId } }),
    rust.client.callTool({ name: "devbox_job_cancel", arguments: { job_id: rustProgramId } }),
  ]);
  compareResults("devbox_job_cancel", jsCancel, rustCancel);
  const jsCancelled = await js.client.callTool({ name: "devbox_job_status", arguments: { job_id: jsProgramId, wait_seconds: 10, terminal_only: true } });
  const rustCancelled = await rust.client.callTool({ name: "devbox_job_status", arguments: { job_id: rustProgramId, wait_seconds: 10, terminal_only: true } });
  compareResults("devbox_job_status", jsCancelled, rustCancelled);
} finally {
  await js.close();
  await rust.close();
  await rm(fixtureRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  await rm(jsStateRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  await rm(rustStateRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  await rm(ghConfigRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
}
console.log(JSON.stringify({ ok: differences.length === 0, callCount: comparedCalls, differingCallCount: differences.length, differingCalls: differences.map((entry) => entry.name), differences }, null, 2));
process.exitCode = differences.length === 0 ? 0 : 2;
