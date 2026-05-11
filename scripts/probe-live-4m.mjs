import { createHash, randomUUID } from "node:crypto";
import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(__dirname, "..");
const runDir = path.join(projectRoot, "run");

const resolveAccessToken = async (mcpUrl) => {
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
    const refreshEntry = [...(oauthState.refreshTokens ?? [])]
      .map(([token, record]) => ({ token, record }))
      .filter(({ record }) => Number(record?.expiresAt) > now && record?.clientId)
      .sort((left, right) => Number(right.record.expiresAt) - Number(left.record.expiresAt))[0];

    if (!refreshEntry?.token) {
      throw new Error("No valid OAuth access token or refresh token found in run/oauth-state.json.");
    }

    const tokenUrl = new URL("/token", mcpUrl);
    const body = new URLSearchParams({
      grant_type: "refresh_token",
      refresh_token: refreshEntry.token,
      client_id: refreshEntry.record.clientId,
    });
    if (refreshEntry.record.resource) {
      body.set("resource", refreshEntry.record.resource);
    }
    if (Array.isArray(refreshEntry.record.scopes) && refreshEntry.record.scopes.length > 0) {
      body.set("scope", refreshEntry.record.scopes.join(" "));
    }

    const response = await fetch(tokenUrl, {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded" },
      body,
    });
    if (!response.ok) {
      throw new Error(`OAuth refresh failed with HTTP ${response.status}: ${await response.text()}`);
    }

    const tokenResponse = await response.json();
    if (!tokenResponse.access_token) {
      throw new Error("OAuth refresh response did not include an access token.");
    }
    return tokenResponse.access_token;
  }

  return tokenEntry.token;
};

const assert = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};

const mcpUrl = new URL(process.argv[2] ?? "http://127.0.0.1:8100/");
const runId = `${new Date().toISOString().replace(/[:.]/g, "-")}-${randomUUID().slice(0, 8)}`;
const artifactDir = path.join(runDir, "soak", `live-4m-${runId}`);
const filePath = path.join(artifactDir, "four-million.txt");
const payload = "a".repeat(4_000_000);
const payloadSha256 = createHash("sha256").update(payload).digest("hex");
const reportPath = path.join(runDir, "soak", `live-4m-${runId}.json`);
const latestReportPath = path.join(runDir, "soak", "live-4m-latest.json");

await mkdir(artifactDir, { recursive: true });

const accessToken = await resolveAccessToken(mcpUrl);
const transport = new StreamableHTTPClientTransport(mcpUrl, {
  requestInit: {
    headers: {
      Authorization: `Bearer ${accessToken}`,
      "User-Agent": "openai-mcp/1.0.0 live-4m-probe",
    },
  },
});
const client = new Client({ name: "live-4m-probe", version: "1.0.0" });

const startedAt = new Date().toISOString();
let writeResult;
let readResult;
let devboxStatusResult;
let devboxReadonlyResult;

try {
  await client.connect(transport);

  writeResult = await client.callTool({
    name: "windows_host_write_large_file",
    arguments: {
      path: filePath,
      working_dir: artifactDir,
      content: payload,
      create_dirs: true,
      expected_sha256: payloadSha256,
    },
  });

  assert(writeResult.structuredContent?.ok === true, writeResult.structuredContent?.summary ?? "4M write failed");
  assert(writeResult.structuredContent?.data?.bytes_written === payload.length, "4M write reported an unexpected byte count");
  assert(writeResult.structuredContent?.data?.content_sha256 === payloadSha256, "4M write reported an unexpected SHA-256");

  readResult = await client.callTool({
    name: "windows_host_read_large_file",
    arguments: {
      path: filePath,
      working_dir: artifactDir,
      offset_bytes: 0,
      max_bytes: payload.length,
    },
  });

  assert(readResult.structuredContent?.ok === true, readResult.structuredContent?.summary ?? "4M read failed");
  assert(readResult.structuredContent?.data?.bytes_returned === payload.length, "4M read reported an unexpected byte count");
  assert(readResult.structuredContent?.data?.content_sha256 === payloadSha256, "4M read reported an unexpected SHA-256");
  assert(typeof readResult.structuredContent?.data?.content_base64 === "string", "4M read did not return base64 content");

  devboxStatusResult = await client.callTool({
    name: "devbox_status",
    arguments: {},
  });
  assert(devboxStatusResult.structuredContent?.ok === true, devboxStatusResult.structuredContent?.summary ?? "devbox_status failed");
  assert(devboxStatusResult.structuredContent?.data?.running === true, "devbox_status did not report a running devbox");

  devboxReadonlyResult = await client.callTool({
    name: "devbox_exec_readonly",
    arguments: {
      command: "printf live-devbox-ready",
      working_dir: "/workspace",
      timeout_seconds: 30,
      user: "root",
    },
  });
  assert(
    devboxReadonlyResult.structuredContent?.ok === true,
    devboxReadonlyResult.structuredContent?.summary ?? "devbox_exec_readonly failed",
  );
  assert((devboxReadonlyResult.structuredContent?.stdout ?? "").includes("live-devbox-ready"), "devbox readonly probe returned unexpected stdout");
} finally {
  await client.close().catch(() => {});
  await rm(artifactDir, { recursive: true, force: true });
}

const report = {
  ok: true,
  started_at: startedAt,
  finished_at: new Date().toISOString(),
  mcp_url: mcpUrl.toString(),
  payload_chars: payload.length,
  payload_sha256: payloadSha256,
  write: {
    bytes_written: writeResult.structuredContent.data.bytes_written,
    content_sha256: writeResult.structuredContent.data.content_sha256,
  },
  read: {
    bytes_returned: readResult.structuredContent.data.bytes_returned,
    content_sha256: readResult.structuredContent.data.content_sha256,
    content_base64_length: readResult.structuredContent.data.content_base64.length,
  },
  devbox: {
    running: devboxStatusResult.structuredContent.data.running,
    readonly_stdout: devboxReadonlyResult.structuredContent.stdout,
  },
  report_path: reportPath,
};

await mkdir(path.dirname(reportPath), { recursive: true });
await writeFile(reportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
await writeFile(latestReportPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
