import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { mkdtemp, readFile, writeFile } from "node:fs/promises";

const importFresh = async (relativePath) => {
  const href = pathToFileURL(path.join(process.cwd(), relativePath)).href;
  return import(`${href}?t=${Date.now()}-${Math.random()}`);
};

test("host runtime reports ready status and creates the workspace directory", async () => {
  const workspaceDir = await mkdtemp(path.join(os.tmpdir(), "devbox-host-runtime-"));
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = path.join(workspaceDir, "workspace");
  process.env.DEVBOX_WORKSPACE_PATH = process.env.HOST_WORKSPACE_PATH;

  const { ensureHostRuntimeReady } = await importFresh("src/host-runtime.js");
  const info = await ensureHostRuntimeReady();

  assert.equal(info.mode, "host");
  assert.equal(info.exists, true);
  assert.equal(info.running, true);
  assert.equal(info.workspacePath, process.env.HOST_WORKSPACE_PATH);
});

test("host runtime can run shell commands and perform file operations", async () => {
  const workspaceDir = await mkdtemp(path.join(os.tmpdir(), "devbox-host-runtime-"));
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspaceDir;
  process.env.DEVBOX_WORKSPACE_PATH = workspaceDir;
  process.env.HOST_DEFAULT_WORKDIR = workspaceDir;
  delete process.env.HOST_SHELL;

  const {
    execInHostRuntime,
    listFilesInHostRuntime,
    readFileInHostRuntime,
    searchFilesInHostRuntime,
    writeFileInHostRuntime,
  } = await importFresh("src/host-runtime.js");

  await writeFile(path.join(workspaceDir, "notes.txt"), "alpha\nbeta\nbeta\n", "utf8");
  const command = process.platform === "win32" ? "[Console]::Out.Write('host-runtime-ok')" : "printf 'host-runtime-ok'";
  const execResult = await execInHostRuntime({ command, workingDir: workspaceDir, timeoutMs: 5000 });
  const listResult = await listFilesInHostRuntime({ path: workspaceDir, recursive: true, maxDepth: 2 });
  const readResult = await readFileInHostRuntime({ path: path.join(workspaceDir, "notes.txt"), maxBytes: 64 });
  const searchResult = await searchFilesInHostRuntime({ pattern: "beta", path: workspaceDir, maxMatches: 10 });
  await writeFileInHostRuntime({ path: path.join(workspaceDir, "written.txt"), content: "hello host runtime" });

  assert.equal(execResult.stdout, "host-runtime-ok");
  assert.match(listResult.stdout, /notes\.txt/);
  assert.equal(readResult.stdout, "alpha\nbeta\nbeta\n");
  assert.equal(searchResult.stdout.match(/notes\.txt/g)?.length, 2);
  assert.equal(await readFile(path.join(workspaceDir, "written.txt"), "utf8"), "hello host runtime");
});
