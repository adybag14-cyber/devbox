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
} = {}) => {
  const port = await getFreePort();
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
    },
    stdio: ["ignore", "pipe", "pipe"],
  });

  collectStream(child.stdout, stdout);
  collectStream(child.stderr, stderr);
  t.after(async () => {
    await terminateChild(child);
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
