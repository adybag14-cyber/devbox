import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { mkdir, mkdtemp, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";

const hasRg = spawnSync("rg", ["--version"], { stdio: "ignore" }).status === 0;

test("host search uses ripgrep fast path when available", { skip: !hasRg }, async () => {
  const workspace = await mkdtemp(path.join(os.tmpdir(), "devbox-rg-search-"));
  await mkdir(path.join(workspace, "node_modules", "ignored"), { recursive: true });
  await writeFile(path.join(workspace, "alpha.js"), "first\nneedle alpha\n", "utf8");
  await writeFile(path.join(workspace, "beta.js"), "needle beta\n", "utf8");
  await writeFile(path.join(workspace, "node_modules", "ignored", "bad.js"), "needle ignored\n", "utf8");
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspace;
  process.env.DEVBOX_WORKSPACE_PATH = workspace;
  process.env.HOST_SEARCH_BACKEND = "rg";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-runtime.js")).href;
  const { searchFilesInHostRuntime } = await import(`${href}?rg=${Date.now()}-${Math.random()}`);
  const result = await searchFilesInHostRuntime({
    pattern: "needle",
    path: workspace,
    glob: "*.js",
    maxMatches: 10,
    maxDepth: 8,
  });
  assert.match(result.stderr, /search backend ripgrep/u);
  assert.match(result.stdout, /alpha\.js:2:needle alpha/u);
  assert.match(result.stdout, /beta\.js:1:needle beta/u);
  assert.doesNotMatch(result.stdout, /node_modules/u);
});


test("ripgrep host search honors the file scan ceiling", { skip: !hasRg }, async () => {
  const workspace = await mkdtemp(path.join(os.tmpdir(), "devbox-rg-limit-"));
  for (let index = 0; index < 12; index += 1) {
    await writeFile(path.join(workspace, `file-${String(index).padStart(2, "0")}.txt`), `line ${index}\n`, "utf8");
  }
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspace;
  process.env.DEVBOX_WORKSPACE_PATH = workspace;
  process.env.HOST_SEARCH_BACKEND = "rg";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-runtime.js")).href;
  const { searchFilesInHostRuntime } = await import(`${href}?rg-limit=${Date.now()}-${Math.random()}`);
  const result = await searchFilesInHostRuntime({
    pattern: "this-pattern-does-not-exist",
    path: workspace,
    glob: "*.txt",
    maxMatches: 10,
    maxDepth: 8,
    maxFiles: 2,
    timeoutMs: 5000,
  });
  assert.match(result.stderr, /search backend ripgrep/u);
  assert.match(result.stderr, /file scan limit 2 reached/u);
});


test("ripgrep host search respects ignore files by default and supports explicit exhaustive mode", { skip: !hasRg }, async () => {
  const workspace = await mkdtemp(path.join(os.tmpdir(), "devbox-rg-ignore-"));
  await writeFile(path.join(workspace, ".ignore"), "ignored.txt\n.hidden.txt\n", "utf8");
  await writeFile(path.join(workspace, "visible.txt"), "needle visible\n", "utf8");
  await writeFile(path.join(workspace, "ignored.txt"), "needle ignored\n", "utf8");
  await writeFile(path.join(workspace, ".hidden.txt"), "needle hidden\n", "utf8");
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspace;
  process.env.DEVBOX_WORKSPACE_PATH = workspace;
  process.env.HOST_SEARCH_BACKEND = "rg";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-runtime.js")).href;
  const { searchFilesInHostRuntime } = await import(`${href}?rg-ignore=${Date.now()}-${Math.random()}`);

  const normal = await searchFilesInHostRuntime({ pattern: "needle", path: workspace, glob: "*.txt", maxMatches: 10 });
  assert.match(normal.stdout, /visible\.txt/u);
  assert.doesNotMatch(normal.stdout, /ignored\.txt/u);
  assert.doesNotMatch(normal.stdout, /\.hidden\.txt/u);

  const exhaustive = await searchFilesInHostRuntime({
    pattern: "needle", path: workspace, glob: "*.txt", maxMatches: 10, includeIgnored: true,
  });
  assert.match(exhaustive.stdout, /visible\.txt/u);
  assert.match(exhaustive.stdout, /ignored\.txt/u);
  assert.match(exhaustive.stdout, /\.hidden\.txt/u);
});


test("ripgrep host search returns immediately when no candidate files match", { skip: !hasRg }, async () => {
  const workspace = await mkdtemp(path.join(os.tmpdir(), "devbox-rg-empty-"));
  await writeFile(path.join(workspace, "only.txt"), "needle\n", "utf8");
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspace;
  process.env.DEVBOX_WORKSPACE_PATH = workspace;
  process.env.HOST_SEARCH_BACKEND = "rg";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-runtime.js")).href;
  const { searchFilesInHostRuntime } = await import(`${href}?rg-empty=${Date.now()}-${Math.random()}`);
  const result = await searchFilesInHostRuntime({ pattern: "needle", path: workspace, glob: "*.zig", timeoutMs: 5000 });
  assert.equal(result.stdout, "");
  assert.match(result.stderr, /search backend ripgrep/u);
  assert.match(result.stderr, /candidate files 0/u);
});

test("ripgrep host search stops at the global match limit", { skip: !hasRg }, async () => {
  const workspace = await mkdtemp(path.join(os.tmpdir(), "devbox-rg-matches-"));
  const lines = Array.from({ length: 50 }, (_value, index) => `needle ${index}`).join("\n");
  await writeFile(path.join(workspace, "many.txt"), `${lines}\n`, "utf8");
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspace;
  process.env.DEVBOX_WORKSPACE_PATH = workspace;
  process.env.HOST_SEARCH_BACKEND = "rg";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-runtime.js")).href;
  const { searchFilesInHostRuntime } = await import(`${href}?rg-matches=${Date.now()}-${Math.random()}`);
  const result = await searchFilesInHostRuntime({ pattern: "needle", path: workspace, glob: "*.txt", maxMatches: 5, timeoutMs: 5000 });
  assert.match(result.stderr, /search backend ripgrep/u);
  assert.match(result.stderr, /match limit 5 reached/u);
  assert.equal(result.stdout.trim().split("\n").length, 5);
});

test("ripgrep host search preserves JS invalid-regex literal fallback semantics", { skip: !hasRg }, async () => {
  const workspace = await mkdtemp(path.join(os.tmpdir(), "devbox-rg-literal-"));
  await writeFile(path.join(workspace, "literal.txt"), "prefix [ suffix\nno match\n", "utf8");
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  process.env.DEVBOX_RUNTIME_MODE = "host";
  process.env.HOST_WORKSPACE_PATH = workspace;
  process.env.DEVBOX_WORKSPACE_PATH = workspace;
  process.env.HOST_SEARCH_BACKEND = "rg";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-runtime.js")).href;
  const { searchFilesInHostRuntime } = await import(`${href}?rg-literal=${Date.now()}-${Math.random()}`);
  const result = await searchFilesInHostRuntime({ pattern: "[", path: workspace, glob: "*.txt", maxMatches: 10, timeoutMs: 5000 });
  assert.match(result.stderr, /search backend ripgrep/u);
  assert.match(result.stdout, /literal\.txt:1:prefix \[ suffix/u);
});
