import { createHash, randomUUID } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

import { summarizeTelemetry } from "./summarize-mcp-telemetry.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(__dirname, "..");
const runDir = path.join(projectRoot, "run");

const parseArgs = (argv) => {
  const args = {};

  for (let index = 0; index < argv.length; index += 1) {
    const entry = argv[index];
    if (!entry.startsWith("--")) {
      continue;
    }

    const key = entry.slice(2);
    const next = argv[index + 1];
    if (!next || next.startsWith("--")) {
      args[key] = true;
      continue;
    }

    args[key] = next;
    index += 1;
  }

  return args;
};

const truncateString = (value, maxLength = 320) =>
  typeof value === "string" && value.length > maxLength ? `${value.slice(0, maxLength)}...` : value;

const summarizeValue = (value, depth = 0) => {
  if (value === null || value === undefined) {
    return value ?? null;
  }

  if (typeof value === "string") {
    return truncateString(value);
  }

  if (typeof value !== "object") {
    return value;
  }

  if (depth >= 3) {
    return Array.isArray(value) ? `[array(${value.length})]` : "[object]";
  }

  if (Array.isArray(value)) {
    return value.slice(0, 10).map((entry) => summarizeValue(entry, depth + 1));
  }

  return Object.fromEntries(
    Object.entries(value)
      .slice(0, 16)
      .map(([key, entry]) => [key, summarizeValue(entry, depth + 1)]),
  );
};

const sha256Hex = (value) => createHash("sha256").update(value).digest("hex");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const resolveAccessToken = async () => {
  if (process.env.MCP_ACCESS_TOKEN?.trim()) {
    return process.env.MCP_ACCESS_TOKEN.trim();
  }

  const oauthState = JSON.parse(await readFile(path.join(runDir, "oauth-state.json"), "utf8"));
  const now = Date.now();
  const tokenEntry = [...(oauthState.accessTokens ?? [])]
    .map(([token, record]) => ({ token, record }))
    .filter(({ record }) => Number(record?.expiresAt) > now)
    .sort((left, right) => Number(right.record.expiresAt) - Number(left.record.expiresAt))[0];

  if (!tokenEntry?.token) {
    throw new Error("No valid OAuth access token found in run/oauth-state.json.");
  }

  return tokenEntry.token;
};

const assert = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};

const defaultWorkingDir = "/workspace";
const defaultUser = "root";

const args = parseArgs(process.argv.slice(2));
const mcpUrl = new URL(String(args.url ?? "http://127.0.0.1:8100/"));
const includeRecreate = !args["skip-recreate"];
const includeSyncGithub = !args["skip-sync-gh"];
const slowMs = Number(args["slow-ms"]) > 0 ? Number(args["slow-ms"]) : 15000;
const runId = `${new Date().toISOString().replace(/[:.]/g, "-")}-${randomUUID().slice(0, 8)}`;
const soakDir = path.join(runDir, "soak");
const reportPath = args.report ? path.resolve(String(args.report)) : path.join(soakDir, `${runId}.json`);
const latestReportPath = path.join(soakDir, "latest.json");
const hostArtifactDir = path.join(soakDir, runId, "artifacts");
const hostScriptPath = path.join(hostArtifactDir, "probe.ps1");
const hostLargeFilePath = path.join(hostArtifactDir, "payload.bin");
const devboxDir = `/workspace/mcp-soak-${runId}`;
const devboxTextPath = `${devboxDir}/sample.txt`;
const devboxLargeFilePath = `${devboxDir}/payload.bin`;
const devboxTmpDir = `/tmp/mcp-soak-${runId}`;
const devboxTmpTextPath = `${devboxTmpDir}/sample.txt`;
const marker = `mcp-soak-${runId}`;
const binaryPayload = Buffer.from(Array.from({ length: 256 }, (_, index) => index));
const binaryPayloadBase64 = binaryPayload.toString("base64");
const binaryPayloadSha256 = sha256Hex(binaryPayload);
const hostScriptContent = `Write-Host '${marker}'\n`;

await mkdir(hostArtifactDir, { recursive: true });

const accessToken = await resolveAccessToken();
const transport = new StreamableHTTPClientTransport(mcpUrl, {
  requestInit: {
    headers: {
      Authorization: `Bearer ${accessToken}`,
      "User-Agent": "openai-mcp/1.0.0 live-soak",
    },
  },
});
const client = new Client({ name: "live-soak", version: "1.0.0" });

const startedAt = new Date().toISOString();
const results = [];
const failures = [];
let finalHealth = null;

const invokeTool = async (name, toolArgs) => client.callTool({ name, arguments: toolArgs });

const runTool = async (name, toolArgs = {}, validate = null) => {
  const started = Date.now();

  try {
    const response = await invokeTool(name, toolArgs);
    const structured = response.structuredContent ?? {};
    const ok = structured.ok ?? !response.isError;
    if (!ok) {
      throw new Error(structured.summary ?? `${name} failed without a summary.`);
    }

    if (validate) {
      await validate(structured, response);
    }

    const record = {
      tool: name,
      ok: true,
      duration_ms: Date.now() - started,
      summary: truncateString(structured.summary ?? null),
      exit_code: structured.exitCode ?? null,
      arguments: summarizeValue(toolArgs),
      data: summarizeValue(structured.data),
      stdout: truncateString(structured.stdout ?? null),
      stderr: truncateString(structured.stderr ?? null),
    };
    results.push(record);
    return record;
  } catch (error) {
    const record = {
      tool: name,
      ok: false,
      duration_ms: Date.now() - started,
      arguments: summarizeValue(toolArgs),
      error: truncateString(error instanceof Error ? error.message : String(error)),
    };
    results.push(record);
    failures.push(record);
    return record;
  }
};

const waitForHealthyDevbox = async (timeoutMs = 180000) => {
  const deadline = Date.now() + timeoutMs;
let lastError = "devbox not ready";

  while (Date.now() < deadline) {
    try {
      const status = await invokeTool("devbox_status", {});
      const statusContent = status.structuredContent ?? {};
      if (statusContent.ok && statusContent.data?.running) {
        const probe = await invokeTool("devbox_exec_readonly", {
          command: "echo devbox-ready",
          working_dir: defaultWorkingDir,
          timeout_seconds: 30,
          user: defaultUser,
        });
        const probeContent = probe.structuredContent ?? {};
        if (probeContent.ok && (probeContent.stdout ?? "").includes("devbox-ready")) {
          return;
        }
      }

      lastError = statusContent.summary ?? "devbox status probe did not report running";
    } catch (error) {
      lastError = error instanceof Error ? error.message : String(error);
    }

    await sleep(3000);
  }

  throw new Error(`Timed out waiting for the devbox to become ready: ${lastError}`);
};

const collectFinalHealth = async () => {
  await waitForHealthyDevbox();
  await sleep(2000);

  const response = await invokeTool("devbox_status", {});
  const structured = response.structuredContent ?? {};
  assert(structured.ok, structured.summary ?? "Final devbox_status probe failed.");
  assert(structured.data?.running === true, "Final devbox_status did not report a running devbox.");

  return summarizeValue(structured.data);
};

await client.connect(transport);

try {
  const { tools } = await client.listTools();
  const toolNames = new Set(tools.map((tool) => tool.name));
  assert(toolNames.has("devbox_status"), "MCP server did not expose devbox_status.");
  assert(toolNames.has("windows_host_exec"), "MCP server did not expose windows_host_exec.");

  await runTool("devbox_status", {}, (structured) => {
    assert(structured.data?.running === true, "devbox_status did not report a running devbox.");
  });

  await runTool("windows_host_status", {}, (structured) => {
    assert(Array.isArray(structured.data?.allowlist), "windows_host_status did not return allowlist.");
  });

  if (includeSyncGithub) {
    await runTool("devbox_sync_github_auth_from_host", {}, (structured) => {
      assert(typeof structured.data?.statusSummary === "string", "GitHub auth sync did not return a status summary.");
    });
  }

  await runTool("devbox_github_auth_status", {}, (structured) => {
    assert(typeof structured.data?.statusSummary === "string", "devbox_github_auth_status did not return statusSummary.");
  });

  await runTool("windows_host_write_large_file", {
    path: hostScriptPath,
    working_dir: hostArtifactDir,
    content: hostScriptContent,
    create_dirs: true,
  }, (structured) => {
    assert(structured.data?.verified === true, "windows_host_write_large_file did not verify the PowerShell probe file.");
  });

  await runTool("windows_host_inspect_file", {
    path: hostScriptPath,
    working_dir: hostArtifactDir,
  }, (structured) => {
    assert(structured.data?.exists === true, "windows_host_inspect_file did not find the PowerShell probe file.");
    assert(structured.data?.syntax_invalid === false, "windows_host_inspect_file marked the probe file as syntactically invalid.");
  });

  await runTool("windows_host_write_large_file", {
    path: hostLargeFilePath,
    working_dir: hostArtifactDir,
    content_base64: binaryPayloadBase64,
    create_dirs: true,
    expected_sha256: binaryPayloadSha256,
  }, (structured) => {
    assert(structured.data?.verified === true, "windows_host_write_large_file did not verify the binary payload.");
    assert(structured.data?.content_sha256 === binaryPayloadSha256, "windows_host_write_large_file reported the wrong SHA-256.");
  });

  await runTool("windows_host_read_large_file", {
    path: hostLargeFilePath,
    working_dir: hostArtifactDir,
    offset_bytes: 0,
    max_bytes: 512,
  }, (structured) => {
    assert(structured.data?.content_sha256 === binaryPayloadSha256, "windows_host_read_large_file returned the wrong SHA-256.");
    assert(structured.data?.content_base64 === binaryPayloadBase64, "windows_host_read_large_file returned the wrong bytes.");
  });

  await runTool("windows_host_run_program", {
    program: "node",
    args: ["-e", "console.log(process.version)"],
    working_dir: hostArtifactDir,
    timeout_seconds: 30,
  }, (structured) => {
    assert(/^v\d+/.test(structured.stdout ?? ""), "windows_host_run_program did not return a Node.js version.");
  });

  await runTool("windows_host_exec", {
    command: "Get-Location | Select-Object -ExpandProperty Path",
    working_dir: hostArtifactDir,
    timeout_seconds: 30,
  }, (structured) => {
    assert((structured.stdout ?? "").trim() === hostArtifactDir, "windows_host_exec did not honor the working directory.");
  });

  await runTool("devbox_exec", {
    command: `mkdir -p '${devboxDir}' && printf '%s\\n' '${marker}' > '${devboxTextPath}'`,
    working_dir: defaultWorkingDir,
    timeout_seconds: 60,
    user: defaultUser,
  });

  await runTool("devbox_exec", {
    command: `mkdir -p '${devboxTmpDir}' && printf '%s\\n' '${marker}' > '${devboxTmpTextPath}'`,
    working_dir: defaultWorkingDir,
    timeout_seconds: 60,
    user: defaultUser,
  });

  await runTool("devbox_write_file", {
    path: devboxTextPath,
    content: `${marker}\nline-2\n`,
    create_dirs: true,
  });

  await runTool("devbox_read_file", {
    path: devboxTextPath,
    max_bytes: 4096,
  }, (structured) => {
    assert((structured.stdout ?? "").includes(marker), "devbox_read_file did not return the written marker.");
  });

  await runTool("devbox_write_large_file", {
    path: devboxLargeFilePath,
    content_base64: binaryPayloadBase64,
    create_dirs: true,
    expected_sha256: binaryPayloadSha256,
  }, (structured) => {
    assert(structured.data?.verified === true, "devbox_write_large_file did not verify the binary payload.");
    assert(structured.data?.content_sha256 === binaryPayloadSha256, "devbox_write_large_file reported the wrong SHA-256.");
  });

  await runTool("devbox_read_large_file", {
    path: devboxLargeFilePath,
    offset_bytes: 0,
    max_bytes: 512,
  }, (structured) => {
    assert(structured.data?.content_sha256 === binaryPayloadSha256, "devbox_read_large_file returned the wrong SHA-256.");
    assert(structured.data?.content_base64 === binaryPayloadBase64, "devbox_read_large_file returned the wrong bytes.");
  });

  await runTool("devbox_list_files", {
    path: devboxDir,
    recursive: true,
    max_depth: 2,
  }, (structured) => {
    assert((structured.stdout ?? "").includes("sample.txt"), "devbox_list_files did not list the text probe.");
    assert((structured.stdout ?? "").includes("payload.bin"), "devbox_list_files did not list the binary probe.");
  });

  await runTool("devbox_search_files", {
    pattern: marker,
    path: devboxDir,
    glob: "*",
    max_matches: 20,
  }, (structured) => {
    assert((structured.stdout ?? "").includes(marker), "devbox_search_files did not find the probe marker.");
  });

  await runTool("devbox_exec_readonly", {
    command: `pwd && test -f '${devboxLargeFilePath}' && echo readonly-ok`,
    working_dir: devboxDir,
    timeout_seconds: 60,
    user: defaultUser,
  }, (structured) => {
    assert((structured.stdout ?? "").includes("readonly-ok"), "devbox_exec_readonly did not confirm the probe files.");
  });

  await runTool("devbox_exec", {
    command: `printf '%s\\n' 'exec-ok' > '${devboxDir}/exec.txt' && cat '${devboxDir}/exec.txt'`,
    working_dir: defaultWorkingDir,
    timeout_seconds: 60,
    user: defaultUser,
  }, (structured) => {
    assert((structured.stdout ?? "").includes("exec-ok"), "devbox_exec did not return the command output.");
  });

  await runTool("devbox_restart", {}, async () => {
    await waitForHealthyDevbox();
  });

  await runTool("devbox_status", {}, (structured) => {
    assert(structured.data?.running === true, "devbox_status did not report a running devbox after restart.");
  });

  await runTool("devbox_stop", {}, async () => {
    await sleep(2000);
  });

  await runTool("devbox_status", {}, (structured) => {
    assert(typeof structured.data?.running === "boolean", "devbox_status did not return a boolean running state after stop.");
  });

  await runTool("devbox_start", {}, async () => {
    await waitForHealthyDevbox();
  });

  await runTool("devbox_status", {}, (structured) => {
    assert(structured.data?.running === true, "devbox_status did not report a running devbox after start.");
  });

  if (includeRecreate) {
    await runTool("devbox_recreate", {}, async () => {
      await waitForHealthyDevbox();
    });

    await runTool("devbox_read_file", {
      path: devboxTextPath,
      max_bytes: 4096,
    }, (structured) => {
      assert((structured.stdout ?? "").includes(marker), "Workspace contents were not preserved across devbox_recreate.");
    });

    await runTool("devbox_read_file", {
      path: devboxTmpTextPath,
      max_bytes: 4096,
    }, (structured) => {
      assert((structured.stdout ?? "").includes(marker), "/tmp contents were not preserved across devbox_recreate.");
    });
  }

  finalHealth = await collectFinalHealth();
} finally {
  await client.close().catch(() => {});
}

const telemetry = await summarizeTelemetry({
  projectRoot,
  since: startedAt,
  slowMs,
});

const timeoutSuspects = [
  ...telemetry.tools.timeout_suspects,
  ...telemetry.http.error_responses.filter((entry) => /408|504/.test(String(entry.status_code))),
];
const slowCalls = results.filter((entry) => entry.duration_ms >= slowMs);

const report = {
  generated_at: new Date().toISOString(),
  run_id: runId,
  started_at: startedAt,
  mcp_url: mcpUrl.toString(),
  include_recreate: includeRecreate,
  include_sync_github: includeSyncGithub,
  slow_ms: slowMs,
  result_count: results.length,
  failure_count: failures.length,
  timeout_suspect_count: timeoutSuspects.length,
  slow_call_count: slowCalls.length,
  final_health: finalHealth,
  results,
  failures,
  timeout_suspects: timeoutSuspects,
  telemetry,
};

await mkdir(path.dirname(reportPath), { recursive: true });
await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
await writeFile(latestReportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");

process.stdout.write(
  `${JSON.stringify(
    {
      report_path: reportPath,
      result_count: results.length,
      failure_count: failures.length,
      timeout_suspect_count: timeoutSuspects.length,
      slow_call_count: slowCalls.length,
      guardian: telemetry.guardian,
      tool_errors: telemetry.tools.total_errors,
      http_error_count: telemetry.http.error_responses.length,
    },
    null,
    2,
  )}\n`,
);

if (failures.length > 0 || timeoutSuspects.length > 0 || telemetry.tools.total_errors > 0 || telemetry.http.error_responses.length > 0) {
  process.exitCode = 1;
}
