import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { once } from "node:events";
import { access, mkdtemp, rm, writeFile } from "node:fs/promises";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

const getFreePort = () =>
  new Promise((resolve, reject) => {
    const server = net.createServer();

    server.once("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const address = server.address();
      if (!address || typeof address === "string") {
        reject(new Error("Failed to allocate a TCP port for the test server."));
        return;
      }

      server.close((error) => {
        if (error) {
          reject(error);
          return;
        }

        resolve(address.port);
      });
    });
  });

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const waitForHealth = async (port, timeoutMs = 10000) => {
  const deadline = Date.now() + timeoutMs;
  let lastError = null;

  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/healthz`);
      const body = await response.text();
      if (response.ok && body === "ok") {
        return;
      }
    } catch (error) {
      lastError = error;
    }

    await wait(100);
  }

  throw lastError ?? new Error(`Timed out waiting for the MCP server on port ${port}.`);
};

const collectStream = (stream, buffer) => {
  stream.setEncoding("utf8");
  stream.on("data", (chunk) => buffer.push(chunk));
};

const terminateChild = async (child) => {
  if (child.exitCode !== null) {
    return;
  }

  child.kill();
  await Promise.race([
    once(child, "exit"),
    (async () => {
      await wait(5000);
      if (child.exitCode === null) {
        child.kill("SIGKILL");
        await once(child, "exit");
      }
    })(),
  ]);
};

const startServer = async (t, {
  maxTextOutputChars = "20000",
  maxCommandOutputChars = "65536",
  execMaxConcurrent = "16",
  execReservedInteractive = "1",
} = {}) => {
  const port = await getFreePort();
  const executionSlotRoot = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-mcp-slots-"));
  const jobsRoot = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-mcp-jobs-"));
  const stdout = [];
  const stderr = [];
  const child = spawn(process.execPath, ["src/server.js"], {
    cwd: projectRoot,
    env: {
      ...process.env,
      HOST: "127.0.0.1",
      PORT: String(port),
      MCP_AUTH_MODE: "none",
      PUBLIC_BASE_URL: "",
      MAX_TEXT_OUTPUT_CHARS: maxTextOutputChars,
      MAX_COMMAND_OUTPUT_CHARS: maxCommandOutputChars,
      DEVBOX_RUNTIME_MODE: "host",
      HOST_WORKSPACE_PATH: projectRoot,
      HOST_DEFAULT_WORKDIR: projectRoot,
      HOST_PROGRAM_ALLOWLIST: "powershell,pwsh,cmd,git,gh,docker,node,npm,npx,python,py,pip,rg,curl,winget",
      DEVBOX_PROGRAM_ALLOWLIST: "powershell,pwsh,cmd,git,gh,docker,node,npm,npx,python,py,pip,rg,curl,winget",
      MCP_EXEC_SLOT_ROOT: executionSlotRoot,
      MCP_JOBS_ROOT: jobsRoot,
      MCP_EXEC_MAX_CONCURRENT: execMaxConcurrent,
      MCP_EXEC_RESERVED_INTERACTIVE: execReservedInteractive,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });

  collectStream(child.stdout, stdout);
  collectStream(child.stderr, stderr);
  t.after(async () => {
    await terminateChild(child);
    await rm(executionSlotRoot, { recursive: true, force: true });
    await rm(jobsRoot, { recursive: true, force: true });
  });

  await waitForHealth(port);

  return {
    port,
    stdout,
    stderr,
  };
};

const connectClient = async (t, port) => {
  const transport = new StreamableHTTPClientTransport(new URL(`http://127.0.0.1:${port}/`));
  const client = new Client({
    name: "test-client",
    version: "1.0.0",
  });

  await client.connect(transport);
  t.after(async () => {
    await client.close();
  });

  return client;
};

const assertJpegToolResult = (result) => {
  assert.equal(result.isError, undefined);
  assert.equal(result.structuredContent?.ok, true);
  assert.equal(result.structuredContent?.data?.mime_type, "image/jpeg");
  assert.match(result.structuredContent?.data?.sha256 ?? "", /^[a-f0-9]{64}$/);
  assert.ok(result.structuredContent?.data?.width > 0);
  assert.ok(result.structuredContent?.data?.height > 0);

  const image = result.content?.find((entry) => entry.type === "image");
  assert.ok(image, "Expected an MCP image content block.");
  assert.equal(image.mimeType, "image/jpeg");
  const bytes = Buffer.from(image.data, "base64");
  assert.equal(bytes.length, result.structuredContent.data.bytes);
  assert.deepEqual([...bytes.subarray(0, 3)], [0xff, 0xd8, 0xff]);
  assert.deepEqual([...bytes.subarray(bytes.length - 2)], [0xff, 0xd9]);
  assert.equal(createHash("sha256").update(bytes).digest("hex"), result.structuredContent.data.sha256);
};

const startVisibleTestWindow = async (t) => {
  const script = `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = [System.Windows.Forms.Form]::new()
$form.Text = 'Devbox PID Capture Test'
$form.StartPosition = 'Manual'
$form.Location = [System.Drawing.Point]::new(80, 80)
$form.Size = [System.Drawing.Size]::new(640, 360)
$label = [System.Windows.Forms.Label]::new()
$label.Text = 'DEVBOX PID JPEG CAPTURE'
$label.AutoSize = $true
$label.Font = [System.Drawing.Font]::new('Arial', 20)
$label.Location = [System.Drawing.Point]::new(80, 100)
$form.Controls.Add($label)
$form.Add_Shown({ [Console]::Out.WriteLine('ready'); [Console]::Out.Flush() })
[void]$form.ShowDialog()
`;
  const encoded = Buffer.from(script, "utf16le").toString("base64");
  const child = spawn(
    "powershell.exe",
    ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded],
    { cwd: projectRoot, stdio: ["ignore", "pipe", "pipe"], windowsHide: false },
  );
  const stderr = [];
  collectStream(child.stderr, stderr);
  await Promise.race([
    new Promise((resolve) => {
      child.stdout.setEncoding("utf8");
      child.stdout.on("data", (chunk) => {
        if (chunk.includes("ready")) resolve();
      });
    }),
    (async () => {
      await wait(10000);
      throw new Error(`Timed out waiting for the PID capture test window. stderr:\n${stderr.join("")}`);
    })(),
  ]);
  t.after(async () => terminateChild(child));
  return child;
};

const terminateWindowsProcessTree = async (pid) => {
  if (process.platform !== "win32") return;
  const killer = spawn("taskkill.exe", ["/pid", String(pid), "/t", "/f"], {
    cwd: projectRoot,
    stdio: "ignore",
    windowsHide: true,
  });
  await Promise.race([once(killer, "exit"), wait(5000)]);
};

const startChildOwnedTestWindow = async (t) => {
  const script = String.raw`
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = [System.Windows.Forms.Form]::new()
$form.Text = 'Devbox Child PID Capture Test'
$form.StartPosition = 'Manual'
$form.Location = [System.Drawing.Point]::new(180, 180)
$form.Size = [System.Drawing.Size]::new(700, 400)
$form.BackColor = [System.Drawing.Color]::DarkBlue
$form.Add_Shown({ [Console]::Out.WriteLine('ready'); [Console]::Out.Flush() })
[void]$form.ShowDialog()
`;
  const encoded = Buffer.from(script, "utf16le").toString("base64");
  const command = `powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ${encoded}`;
  const launcher = spawn("cmd.exe", ["/d", "/s", "/c", command], {
    cwd: projectRoot,
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: false,
  });
  t.after(async () => terminateWindowsProcessTree(launcher.pid));
  const stderr = [];
  collectStream(launcher.stderr, stderr);
  let readinessTimer;
  try {
    await new Promise((resolve, reject) => {
      launcher.stdout.setEncoding("utf8");
      launcher.stdout.on("data", (chunk) => {
        if (chunk.includes("ready")) resolve();
      });
      readinessTimer = setTimeout(
        () => reject(new Error(`Timed out waiting for child-owned capture window. stderr:
${stderr.join("")}`)),
        10000,
      );
    });
  } finally {
    clearTimeout(readinessTimer);
  }
  return launcher;
};

const startBlackPrintWindow = async (t) => {
  const script = String.raw`
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = [System.Windows.Forms.Form]::new()
$form.Text = 'Devbox Black PrintWindow Test'
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$form.StartPosition = 'Manual'
$form.Location = [System.Drawing.Point]::new(120, 120)
$form.Size = [System.Drawing.Size]::new(720, 420)
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::Black
$header = [System.Windows.Forms.Panel]::new()
$header.Dock = [System.Windows.Forms.DockStyle]::Top
$header.Height = 60
$header.BackColor = [System.Drawing.Color]::Magenta
$form.Controls.Add($header)
$form.Add_Shown({ [Console]::Out.WriteLine('ready'); [Console]::Out.Flush() })
[void]$form.ShowDialog()
`;
  const encoded = Buffer.from(script, "utf16le").toString("base64");
  const child = spawn(
    "powershell.exe",
    ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded],
    { cwd: projectRoot, stdio: ["ignore", "pipe", "pipe"], windowsHide: false },
  );
  t.after(async () => terminateChild(child));
  const stderr = [];
  collectStream(child.stderr, stderr);
  let readinessTimer;
  try {
    await new Promise((resolve, reject) => {
      child.stdout.setEncoding("utf8");
      child.stdout.on("data", (chunk) => {
        if (chunk.includes("ready")) resolve();
      });
      readinessTimer = setTimeout(
        () => reject(new Error(`Timed out waiting for black PrintWindow test window. stderr:
${stderr.join("")}`)),
        10000,
      );
    });
  } finally {
    clearTimeout(readinessTimer);
  }
  return child;
};

test("Windows display capture returns actual JPEG bytes through MCP", { skip: process.platform !== "win32" }, async (t) => {
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "windows_host_capture_display",
    arguments: { quality: 70 },
  });

  assertJpegToolResult(result);
  assert.equal(result.structuredContent.data.capture_mode, "full_display");
});

test("Windows PID program capture returns its actual window as JPEG through MCP", { skip: process.platform !== "win32" }, async (t) => {
  const windowProcess = await startVisibleTestWindow(t);
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "windows_host_capture_program",
    arguments: { pid: windowProcess.pid, quality: 80 },
  });

  assertJpegToolResult(result);
  assert.equal(result.structuredContent.data.capture_mode, "program_pid");
  assert.equal(result.structuredContent.data.pid, windowProcess.pid);
  assert.equal(result.structuredContent.data.window_title, "Devbox PID Capture Test");
});


test("generic host capture tools expose the Windows capture backend", { skip: process.platform !== "win32" }, async (t) => {
  const windowProcess = await startVisibleTestWindow(t);
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const tools = await client.listTools();
  const names = new Set(tools.tools.map((tool) => tool.name));
  assert.equal(names.has("host_capture_display"), true);
  assert.equal(names.has("host_capture_window"), true);
  assert.equal(names.has("windows_host_capture_program"), true);

  const result = await client.callTool({
    name: "host_capture_window",
    arguments: { pid: windowProcess.pid, quality: 80, include_process_tree: true },
  });
  assertJpegToolResult(result);
  assert.equal(result.structuredContent.data.pid, windowProcess.pid);
});

test("Windows window capture follows child processes when the launcher PID owns no GUI", { skip: process.platform !== "win32" }, async (t) => {
  const launcher = await startChildOwnedTestWindow(t);
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "host_capture_window",
    arguments: { pid: launcher.pid, include_process_tree: true, quality: 80 },
  });

  assertJpegToolResult(result);
  assert.equal(result.structuredContent.data.pid, launcher.pid);
  assert.equal(result.structuredContent.data.process_tree_fallback, true);
  assert.notEqual(result.structuredContent.data.window_owner_pid, launcher.pid);
  assert.equal(result.structuredContent.data.window_title, "Devbox Child PID Capture Test");
});

test("Windows capture rejects a successful black PrintWindow frame and falls back to compositor pixels", { skip: process.platform !== "win32" }, async (t) => {
  const windowProcess = await startBlackPrintWindow(t);
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "host_capture_window",
    arguments: { pid: windowProcess.pid, quality: 82 },
  });

  assertJpegToolResult(result);
  assert.equal(result.structuredContent.data.print_window_rejected, true);
  assert.ok(result.structuredContent.data.print_window_near_black_ratio < 0.985);
  assert.ok(result.structuredContent.data.print_window_interior_near_black_ratio >= 0.94);
  assert.equal(result.structuredContent.data.capture_method, "DesktopCompositorCopy(window-bounds)");
  assert.equal(result.structuredContent.data.screen_fallback_may_include_occluders, true);
});

const assertSseProbeResponse = async (response, errorMessage) => {
  assert.equal(response.status, 200, errorMessage);
  assert.match(response.headers.get("content-type") ?? "", /^text\/event-stream\b/i);
  const reader = response.body?.getReader();
  const firstChunk = reader ? await reader.read() : null;
  assert.equal(firstChunk?.done, false);
  assert.match(Buffer.from(firstChunk?.value ?? []).toString("utf8"), /^:\s*mcp-sse-probe\b/);

  await reader?.cancel();
};

test("GET / returns an SSE content type for stream probes", async (t) => {
  const { port, stdout, stderr } = await startServer(t);

  const response = await fetch(`http://127.0.0.1:${port}/`, {
    headers: {
      Accept: "text/event-stream",
    },
    signal: AbortSignal.timeout(5000),
  });

  await assertSseProbeResponse(
    response,
    `Expected GET / to establish an SSE stream.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`,
  );
});

test("GET /mcp returns an SSE content type for stream probes", async (t) => {
  const { port, stdout, stderr } = await startServer(t);

  const response = await fetch(`http://127.0.0.1:${port}/mcp`, {
    headers: {
      Accept: "text/event-stream",
    },
    signal: AbortSignal.timeout(5000),
  });

  await assertSseProbeResponse(
    response,
    `Expected GET /mcp to establish an SSE stream.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`,
  );
});

test("POST / accepts MCP initialize requests on the canonical root endpoint", async (t) => {
  const { port, stdout, stderr } = await startServer(t);

  const response = await fetch(`http://127.0.0.1:${port}/`, {
    method: "POST",
    headers: {
      Accept: "application/json, text/event-stream",
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: 1,
      method: "initialize",
      params: {
        protocolVersion: "2025-11-25",
        capabilities: {},
        clientInfo: {
          name: "test-client",
          version: "1.0.0",
        },
      },
    }),
    signal: AbortSignal.timeout(5000),
  });

  const body = await response.text();
  const ssePayloadLine = body
    .split(/\r?\n/)
    .find((line) => line.startsWith("data: "));

  assert.ok(
    ssePayloadLine,
    `Expected POST / initialize to return an SSE data frame.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}\nbody:\n${body}`,
  );

  const payload = JSON.parse(ssePayloadLine.slice("data: ".length));
  assert.equal(
    response.status,
    200,
    `Expected POST / initialize to succeed.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}\nbody:\n${body}`,
  );
  assert.match(response.headers.get("content-type") ?? "", /^text\/event-stream\b/i);
  assert.equal(payload.jsonrpc, "2.0");
  assert.equal(payload.id, 1);
  assert.equal(typeof payload.result?.protocolVersion, "string");
  assert.equal(typeof payload.result?.serverInfo?.name, "string");
});

test("devbox_run_program executes a structured program without shell parsing", async (t) => {
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "node",
      args: ["-e", "process.stdout.write('DIRECT_PROGRAM_OK')"],
      working_dir: projectRoot,
      timeout_seconds: 15,
    },
  });
  assert.equal(result.isError, undefined);
  assert.equal(result.structuredContent?.ok, true);
  assert.equal(result.structuredContent?.stdout, "DIRECT_PROGRAM_OK");
  assert.equal(typeof result.structuredContent?.data?.execution?.queue_wait_ms, "number");
  assert.equal(Number.isInteger(result.structuredContent?.data?.execution?.slot), true);
});

test("windows_host_run_program returns bridge diagnostics for corrupted host files", async (t) => {
  const { port, stdout, stderr } = await startServer(t);
  const client = await connectClient(t, port);
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-mcp-http-test-"));
  const scriptPath = path.join(tempDir, "corrupt.ps1");
  const corruptedScript = `Write-Host 'ok'\n${"\u00e2\u20ac\u201d"}\nif ($true {\n`;

  try {
    await writeFile(scriptPath, corruptedScript, "utf8");

    const result = await client.callTool({
      name: "windows_host_run_program",
      arguments: {
        program: "powershell.exe",
        args: ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", scriptPath],
        working_dir: tempDir,
        timeout_seconds: 15,
      },
    });

    assert.equal(result.isError, true);
    assert.equal(result.structuredContent?.ok, false);
    assert.equal(result.structuredContent?.data?.bridge_diagnostics?.suspected_file_integrity_issue, true);
    assert.match(
      result.content?.[0]?.text ?? "",
      /windows_host_write_large_file/,
      `Expected MCP error text to include repair guidance.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`,
    );

    const inspectedPath = result.structuredContent?.data?.bridge_diagnostics?.inspected_paths?.find(
      (entry) => entry.resolved_path === scriptPath,
    );
    assert.ok(
      inspectedPath,
      `Expected the MCP response to include diagnostics for ${scriptPath}.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}\nresponse:\n${JSON.stringify(result, null, 2)}`,
    );
    assert.equal(inspectedPath.likely_corrupted_on_disk, true);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("MCP accepts and returns 4 million character host-file payloads", async (t) => {
  const { port, stdout, stderr } = await startServer(t);
  const client = await connectClient(t, port);
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-mcp-http-test-"));
  const filePath = path.join(tempDir, "four-million.txt");
  const payload = "a".repeat(4_000_000);
  const payloadSha256 = createHash("sha256").update(payload).digest("hex");

  try {
    const writeResult = await client.callTool({
      name: "windows_host_write_large_file",
      arguments: {
        path: filePath,
        working_dir: tempDir,
        content: payload,
        create_dirs: true,
        expected_sha256: payloadSha256,
      },
    });

    assert.equal(
      writeResult.structuredContent?.ok,
      true,
      `Expected 4M write through MCP to succeed.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}\nresponse:\n${JSON.stringify(writeResult, null, 2)}`,
    );
    assert.equal(writeResult.structuredContent?.data?.bytes_written, payload.length);
    assert.equal(writeResult.structuredContent?.data?.content_sha256, payloadSha256);

    const readResult = await client.callTool({
      name: "windows_host_read_large_file",
      arguments: {
        path: filePath,
        working_dir: tempDir,
        offset_bytes: 0,
        max_bytes: payload.length,
      },
    });

    assert.equal(
      readResult.structuredContent?.ok,
      true,
      `Expected 4M read through MCP to succeed.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}\nresponse summary:\n${readResult.structuredContent?.summary}`,
    );
    assert.equal(readResult.structuredContent?.data?.bytes_returned, payload.length);
    assert.equal(readResult.structuredContent?.data?.content_sha256, payloadSha256);
    assert.equal(typeof readResult.structuredContent?.data?.content_base64, "string");
    assert.ok(readResult.structuredContent.data.content_base64.length >= payload.length);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("parallel same-path host writes are serialized by the MCP boundary", async (t) => {
  const { port, stdout, stderr } = await startServer(t);
  const client = await connectClient(t, port);
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-mcp-http-test-"));
  const filePath = path.join(tempDir, "parallel-append.txt");
  const payloadA = "A".repeat(200_000);
  const payloadB = "B".repeat(200_000);

  try {
    const [first, second] = await Promise.all([
      client.callTool({
        name: "windows_host_write_large_file",
        arguments: {
          path: filePath,
          working_dir: tempDir,
          content: payloadA,
          append: true,
          create_dirs: true,
        },
      }),
      client.callTool({
        name: "windows_host_write_large_file",
        arguments: {
          path: filePath,
          working_dir: tempDir,
          content: payloadB,
          append: true,
          create_dirs: true,
        },
      }),
    ]);

    assert.equal(first.structuredContent?.ok, true, `First parallel write failed.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`);
    assert.equal(second.structuredContent?.ok, true, `Second parallel write failed.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`);

    const readResult = await client.callTool({
      name: "windows_host_read_large_file",
      arguments: {
        path: filePath,
        working_dir: tempDir,
        offset_bytes: 0,
        max_bytes: payloadA.length + payloadB.length,
      },
    });

    const actualSha256 = readResult.structuredContent?.data?.content_sha256;
    const expectedForwardSha256 = createHash("sha256").update(`${payloadA}${payloadB}`).digest("hex");
    const expectedReverseSha256 = createHash("sha256").update(`${payloadB}${payloadA}`).digest("hex");

    assert.equal(readResult.structuredContent?.ok, true);
    assert.equal(readResult.structuredContent?.data?.bytes_returned, payloadA.length + payloadB.length);
    assert.ok(
      actualSha256 === expectedForwardSha256 || actualSha256 === expectedReverseSha256,
      `Parallel writes produced unexpected bytes. SHA-256=${actualSha256}`,
    );
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("oversized failing command output returns a bounded MCP error", async (t) => {
  const { port, stdout, stderr } = await startServer(t);
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "devbox_exec_readonly",
    arguments: {
      command: "$value = 'x' * 250000; [Console]::Out.Write(($value -join '')); exit 7",
      working_dir: projectRoot,
      timeout_seconds: 15,
    },
  });

  assert.equal(result.isError, true);
  assert.equal(result.structuredContent?.ok, false);
  assert.equal(result.structuredContent?.exitCode, 7);
  assert.equal(result.structuredContent?.truncated, true);
  assert.ok((result.structuredContent?.summary?.length ?? Infinity) <= 4096);
  assert.ok((result.structuredContent?.stdout?.length ?? Infinity) <= 20000);
  assert.ok(
    (result.content?.[0]?.text?.length ?? Infinity) < 30000,
    `Expected bounded MCP response.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`,
  );
});

test("unlimited text configuration still returns connector-safe successful command output", async (t) => {
  const { port, stdout, stderr } = await startServer(t, {
    maxTextOutputChars: "unlimited",
    maxCommandOutputChars: "99999999",
  });
  const client = await connectClient(t, port);
  const result = await client.callTool({
    name: "devbox_exec_readonly",
    arguments: {
      command: "$value = 'x' * 900000; [Console]::Out.Write(($value -join ''))",
      working_dir: projectRoot,
      timeout_seconds: 15,
    },
  });

  assert.equal(result.isError, undefined);
  assert.equal(result.structuredContent?.ok, true);
  assert.equal(result.structuredContent?.truncated, true);
  assert.ok((result.structuredContent?.stdout?.length ?? Infinity) <= 65536);
  assert.ok(
    (result.content?.[0]?.text?.length ?? Infinity) < 70000,
    `Expected connector-safe MCP response.\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`,
  );
});

test("disconnecting an MCP call cancels its host command before side effects", async (t) => {
  const { port } = await startServer(t);
  const client = await connectClient(t, port);
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-cancel-test-"));
  const markerPath = path.join(tempDir, "should-not-exist.txt");
  const escapedMarkerPath = markerPath.replaceAll("'", "''");
  const controller = new AbortController();

  try {
    const pending = client.callTool(
      {
        name: "devbox_exec",
        arguments: {
          command: `Start-Sleep -Milliseconds 1500; Set-Content -LiteralPath '${escapedMarkerPath}' -Value 'late side effect'`,
          working_dir: tempDir,
          timeout_seconds: 15,
        },
      },
      undefined,
      { signal: controller.signal },
    );
    setTimeout(() => controller.abort(new Error("test disconnect")), 200);
    const outcome = await pending.then(
      (result) => ({ result }),
      (error) => ({ error }),
    );
    assert.ok(outcome.error || outcome.result?.isError, "Expected the cancelled call to reject or return an MCP error result.");
    if (outcome.result) {
      assert.match(outcome.result.structuredContent?.summary ?? "", /cancel/i);
    }
    await wait(2200);
    await assert.rejects(access(markerPath), (error) => error?.code === "ENOENT");
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("async MCP jobs survive the request boundary and support status, logs, and cancellation", async (t) => {
  const { port, stdout, stderr } = await startServer(t);
  const client = await connectClient(t, port);
  const tools = await client.listTools();
  const names = new Set((tools.tools || []).map((tool) => tool.name));
  for (const name of ["devbox_exec_start", "devbox_job_status", "devbox_job_logs", "devbox_job_cancel"]) {
    assert.ok(names.has(name), `Expected async MCP tool ${name} to be registered.`);
  }
  for (const name of ["devbox_exec", "devbox_exec_readonly", "devbox_run_program", "host_exec", "windows_host_exec"]) {
    const tool = (tools.tools || []).find((entry) => entry.name === name);
    assert.equal(tool?.inputSchema?.properties?.timeout_seconds?.maximum, 90, `${name} should advertise the safe synchronous timeout ceiling.`);
    assert.match(tool?.inputSchema?.properties?.timeout_seconds?.description ?? "", /devbox_exec_start/i);
  }
  const asyncTool = (tools.tools || []).find((entry) => entry.name === "devbox_exec_start");
  assert.equal(asyncTool?.inputSchema?.properties?.timeout_seconds?.maximum, 86400);

  const started = await client.callTool({
    name: "devbox_exec_start",
    arguments: {
      command: `node -e "setTimeout(()=>console.log('ASYNC_DONE'),250)"`,
      working_dir: projectRoot,
      timeout_seconds: 30,
      read_only: true,
    },
  });
  assert.equal(started.structuredContent?.ok, true, `async start failed\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`);
  const jobId = started.structuredContent?.data?.id;
  assert.match(jobId ?? "", /^job-/u);

  let status = null;
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const result = await client.callTool({ name: "devbox_job_status", arguments: { job_id: jobId } });
    assert.equal(result.structuredContent?.ok, true);
    status = result.structuredContent?.data;
    if (["succeeded", "failed", "cancelled", "timed_out"].includes(status?.status)) break;
    await wait(100);
  }
  assert.equal(status?.status, "succeeded", `async job did not succeed: ${JSON.stringify(status)}`);

  const logs = await client.callTool({
    name: "devbox_job_logs",
    arguments: { job_id: jobId, max_chars: 5000 },
  });
  assert.equal(logs.structuredContent?.ok, true);
  assert.match(logs.structuredContent?.data?.stdout ?? "", /ASYNC_DONE/u);

  const longStarted = await client.callTool({
    name: "devbox_exec_start",
    arguments: {
      command: `node -e "setTimeout(()=>console.log('TOO_LATE'),30000)"`,
      working_dir: projectRoot,
      timeout_seconds: 60,
    },
  });
  assert.equal(longStarted.structuredContent?.ok, true);
  const longJobId = longStarted.structuredContent?.data?.id;
  assert.match(longJobId ?? "", /^job-/u);
  const cancelled = await client.callTool({
    name: "devbox_job_cancel",
    arguments: { job_id: longJobId },
  });
  assert.equal(cancelled.structuredContent?.ok, true);
  assert.equal(cancelled.structuredContent?.data?.status, "cancelled");
});


test("round-two wait, output shaping, and structured async tools work end to end", async (t) => {
  const { port, stdout, stderr } = await startServer(t);
  const client = await connectClient(t, port);
  const listed = await client.listTools();
  const tools = new Map((listed.tools || []).map((tool) => [tool.name, tool]));
  for (const name of ["devbox_wait", "devbox_wait_for_file", "devbox_run_program_start"]) {
    assert.ok(tools.has(name), `Expected round-two tool ${name}.`);
  }
  assert.ok(tools.get("devbox_job_status")?.inputSchema?.properties?.wait_seconds);
  assert.ok(tools.get("devbox_exec_readonly")?.inputSchema?.properties?.output_mode);
  assert.ok(tools.get("host_exec")?.inputSchema?.properties?.output_mode);

  const waitStarted = Date.now();
  const waited = await client.callTool({ name: "devbox_wait", arguments: { seconds: 0.1, reason: "integration-test" } });
  assert.equal(waited.structuredContent?.ok, true);
  assert.ok(Date.now() - waitStarted < 2000);
  assert.equal(waited.structuredContent?.data?.execution, undefined);

  const fileDir = await mkdtemp(path.join(os.tmpdir(), "devbox-round2-mcp-file-"));
  const readyFile = path.join(fileDir, "ready.txt");
  t.after(() => rm(fileDir, { recursive: true, force: true }));
  setTimeout(() => writeFile(readyFile, "ready", "utf8"), 100);
  const fileWait = await client.callTool({
    name: "devbox_wait_for_file",
    arguments: { path: readyFile, min_bytes: 5, timeout_seconds: 3, poll_ms: 50 },
  });
  assert.equal(fileWait.structuredContent?.ok, true);
  assert.equal(fileWait.structuredContent?.data?.conditionMet, true);

  const shaped = await client.callTool({
    name: "devbox_run_program",
    arguments: {
      program: "node",
      args: ["-e", "for(let i=0;i<20;i++) console.log('LINE-'+i)"],
      working_dir: projectRoot,
      timeout_seconds: 10,
      output_mode: "tail",
      max_output_lines: 3,
      max_output_chars: 2000,
    },
  });
  assert.equal(shaped.structuredContent?.ok, true, `direct output shaping failed\nstdout:\n${stdout.join("")}\nstderr:\n${stderr.join("")}`);
  assert.match(shaped.structuredContent?.stdout ?? "", /LINE-19/u);
  assert.doesNotMatch(shaped.structuredContent?.stdout ?? "", /LINE-0(?:\r?\n|$)/u);
  assert.equal(shaped.structuredContent?.data?.output?.mode, "tail");

  const started = await client.callTool({
    name: "devbox_run_program_start",
    arguments: {
      program: "node",
      args: ["-e", "setTimeout(()=>console.log('DIRECT_ASYNC_OK'),200)"],
      working_dir: projectRoot,
      timeout_seconds: 20,
      resource_class: "light",
    },
  });
  assert.equal(started.structuredContent?.ok, true);
  const jobId = started.structuredContent?.data?.id;
  assert.match(jobId ?? "", /^job-/u);
  const status = await client.callTool({
    name: "devbox_job_status",
    arguments: { job_id: jobId, wait_seconds: 5, terminal_only: true },
  });
  assert.equal(status.structuredContent?.ok, true);
  assert.equal(status.structuredContent?.data?.status, "succeeded");
  assert.equal(status.structuredContent?.data?.mode, "program");
  const logs = await client.callTool({ name: "devbox_job_logs", arguments: { job_id: jobId, max_chars: 2000 } });
  assert.equal(logs.structuredContent?.ok, true);
  assert.match(logs.structuredContent?.data?.stdout ?? "", /DIRECT_ASYNC_OK/u);

  const statusDetails = await client.callTool({ name: "devbox_status", arguments: {} });
  assert.equal(statusDetails.structuredContent?.ok, true);
  assert.ok(statusDetails.structuredContent?.data?.performance?.eventLoop);
  assert.equal(typeof statusDetails.structuredContent?.data?.performance?.process?.pid, "number");
});
