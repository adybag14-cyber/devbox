import assert from "node:assert/strict";
import { readFile, writeFile, mkdtemp, rm } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { spawn } from "node:child_process";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDir, "..", "..");
const serverPath = path.join(projectRoot, "src", "server.js");
const binaryPath = path.join(projectRoot, "rust-mcp", "target", "debug", process.platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp");
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

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

const commonEnv = {
  MCP_AUTH_MODE: "none",
  PUBLIC_BASE_URL: "",
  DEVBOX_RUNTIME_MODE: process.env.SCHEMA_PARITY_RUNTIME_MODE || "host",
  ENABLE_HOST_EXEC: "true",
  MAX_TEXT_OUTPUT_CHARS: "4000000",
  MAX_COMMAND_OUTPUT_CHARS: "65536",
  MAX_MCP_TRANSFER_CHARS: "4000000",
  HOST_SEARCH_BACKEND: "auto",
  HOST_WORKSPACE_PATH: projectRoot,
  HOST_DEFAULT_WORKDIR: projectRoot,
  DEVBOX_WORKSPACE_PATH: projectRoot,
};
Object.assign(process.env, commonEnv);

const listJsTools = async () => {
  const source = (await readFile(serverPath, "utf8")).replace(
    /export \{ app \};/,
    "export { app, buildServer };",
  );
  assert.match(source, /export \{ app, buildServer \};/);
  const instrumentedPath = path.join(
    path.dirname(serverPath),
    `.schema-parity-server-${process.pid}-${Date.now()}.mjs`,
  );
  await writeFile(instrumentedPath, source, "utf8");
  try {
    const module = await import(`${pathToFileURL(instrumentedPath).href}?audit=${Date.now()}`);
    assert.equal(typeof module.buildServer, "function");
    const server = module.buildServer();
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    const client = new Client({ name: "js-schema-audit", version: "1.0.0" });
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    try {
      return (await client.listTools()).tools;
    } finally {
      await client.close();
      await server.close();
    }
  } finally {
    await rm(instrumentedPath, { force: true });
  }
};

const listRustTools = async () => {
  const runtimeDir = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-schema-"));
  const port = await reservePort();
  let stderr = "";
  const server = spawn(binaryPath, [], {
    cwd: projectRoot,
    env: {
      ...process.env,
      ...commonEnv,
      DEVBOX_PROJECT_ROOT: runtimeDir,
      HOST_WORKSPACE_PATH: projectRoot,
      HOST_DEFAULT_WORKDIR: projectRoot,
      MCP_JOBS_ROOT: path.join(runtimeDir, "jobs"),
      MCP_EXEC_SLOT_ROOT: path.join(runtimeDir, "slots"),
      HOST: "127.0.0.1",
      PORT: String(port),
    },
    stdio: ["ignore", "ignore", "pipe"],
    windowsHide: true,
  });
  server.stderr.setEncoding("utf8");
  server.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-16000); });
  const exited = new Promise((resolve, reject) => {
    server.once("error", reject);
    server.once("exit", (code, signal) => resolve({ code, signal }));
  });
  const baseUrl = new URL(`http://127.0.0.1:${port}/`);
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    const early = await Promise.race([exited.then((result) => ({ result })), sleep(100).then(() => null)]);
    if (early) throw new Error(`Rust schema server exited ${JSON.stringify(early.result)}\n${stderr}`);
    try {
      const health = await fetch(new URL("healthz", baseUrl));
      if (health.ok) break;
    } catch {}
  }
  const client = new Client({ name: "rust-schema-audit", version: "1.0.0" });
  await client.connect(new StreamableHTTPClientTransport(baseUrl));
  try {
    return (await client.listTools()).tools;
  } finally {
    await client.close();
    if (server.exitCode === null && server.signalCode === null) {
      server.kill();
      await Promise.race([exited, sleep(5000)]);
    }
    await rm(runtimeDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
};

const simplify = (schema) => {
  if (Array.isArray(schema)) return schema.map(simplify);
  if (!schema || typeof schema !== "object") return schema;
  const result = {};
  for (const key of Object.keys(schema).sort()) {
    if (["description", "title", "$schema", "examples"].includes(key)) continue;
    result[key] = simplify(schema[key]);
  }
  return result;
};

const flattenOptionalNullable = (schema) => {
  if (Array.isArray(schema)) return schema.map(flattenOptionalNullable);
  if (!schema || typeof schema !== "object") return schema;
  if (Array.isArray(schema.anyOf)) {
    const nonNull = schema.anyOf.filter((item) => !(item && item.type === "null"));
    if (nonNull.length === 1 && nonNull.length !== schema.anyOf.length) {
      return flattenOptionalNullable(nonNull[0]);
    }
  }
  return Object.fromEntries(Object.entries(schema).map(([key, value]) => [key, flattenOptionalNullable(value)]));
};

const normalizeInput = (schema) => simplify(flattenOptionalNullable(schema));
const normalizeOutput = (schema) => simplify(schema);

const jsTools = await listJsTools();
const rustTools = await listRustTools();
const jsByName = new Map(jsTools.map((tool) => [tool.name, tool]));
const rustByName = new Map(rustTools.map((tool) => [tool.name, tool]));
assert.deepEqual([...rustByName.keys()].sort(), [...jsByName.keys()].sort());

const comparableMetadata = (tool) => ({
  title: tool.title ?? null,
  description: tool.description ?? null,
  annotations: tool.annotations ?? null,
});

const inputDifferences = [];
const outputDifferences = [];
const metadataDifferences = [];
for (const name of [...jsByName.keys()].sort()) {
  const jsTool = jsByName.get(name);
  const rustTool = rustByName.get(name);
  const jsInput = normalizeInput(jsTool.inputSchema);
  const rustInput = normalizeInput(rustTool.inputSchema);
  if (JSON.stringify(jsInput) !== JSON.stringify(rustInput)) {
    inputDifferences.push({ name, js: jsInput, rust: rustInput });
  }

  const jsOutput = normalizeOutput(jsTool.outputSchema);
  const rustOutput = normalizeOutput(rustTool.outputSchema);
  if (JSON.stringify(jsOutput) !== JSON.stringify(rustOutput)) {
    outputDifferences.push({ name, js: jsOutput, rust: rustOutput });
  }

  const jsMetadata = comparableMetadata(jsTool);
  const rustMetadata = comparableMetadata(rustTool);
  if (JSON.stringify(jsMetadata) !== JSON.stringify(rustMetadata)) {
    metadataDifferences.push({ name, js: jsMetadata, rust: rustMetadata });
  }
}
const ok = inputDifferences.length === 0 && outputDifferences.length === 0 && metadataDifferences.length === 0;
console.log(JSON.stringify({
  ok,
  toolCount: jsTools.length,
  input: {
    differingToolCount: inputDifferences.length,
    differingTools: inputDifferences.map((entry) => entry.name),
    differences: inputDifferences,
  },
  output: {
    differingToolCount: outputDifferences.length,
    differingTools: outputDifferences.map((entry) => entry.name),
    differences: outputDifferences,
  },
  metadata: {
    differingToolCount: metadataDifferences.length,
    differingTools: metadataDifferences.map((entry) => entry.name),
    differences: metadataDifferences,
  },
}, null, 2));
process.exitCode = ok ? 0 : 2;
