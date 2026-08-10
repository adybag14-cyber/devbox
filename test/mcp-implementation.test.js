import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";

import {
  getMcpLaunchSpec,
  getRustMcpBinaryPath,
  getRustTargetDir,
  prepareMcpImplementation,
  runCheckedProcess,
  resolveCargoCommand,
  resolveMcpImplementation,
} from "../src/mcp-implementation.js";

test("MCP implementation defaults to Rust and keeps JS as explicit rollback", () => {
  assert.equal(resolveMcpImplementation({}), "rust");
  assert.equal(resolveMcpImplementation({ DEVBOX_MCP_IMPLEMENTATION: " RUST " }), "rust");
  assert.equal(resolveMcpImplementation({ DEVBOX_MCP_IMPLEMENTATION: "js" }), "js");
  assert.throws(
    () => resolveMcpImplementation({ DEVBOX_MCP_IMPLEMENTATION: "auto" }),
    /expected rust or js/u,
  );
});

test("Rust launch spec uses release binary and pins the project root", () => {
  const root = path.resolve("/tmp/devbox-cutover");
  const env = { PATH: "test" };
  const posix = getMcpLaunchSpec(root, { env, platform: "linux", implementation: "rust" });
  assert.equal(posix.file, path.join(root, "rust-mcp", "target", "release", "devbox-mcp"));
  assert.deepEqual(posix.args, []);
  assert.equal(posix.env.DEVBOX_PROJECT_ROOT, root);
  assert.equal(posix.env.DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE, "1");
  assert.equal(posix.env.PATH, "test");

  const windows = getMcpLaunchSpec(root, { env, platform: "win32", implementation: "rust" });
  assert.equal(windows.file, path.join(root, "rust-mcp", "target", "release", "devbox-mcp.exe"));
});

test("JS rollback launch spec preserves the existing server entrypoint", () => {
  const root = path.resolve("/tmp/devbox-cutover-js");
  const env = { DEVBOX_MCP_IMPLEMENTATION: "js" };
  const spec = getMcpLaunchSpec(root, { env, implementation: "js" });
  assert.equal(spec.file, process.execPath);
  assert.deepEqual(spec.args, [path.join(root, "src", "server.js")]);
  assert.equal(spec.env, env);
});

test("Rust preflight builds locked release before probing the binary", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-cutover-test-"));
  const binary = getRustMcpBinaryPath(root);
  const calls = [];
  try {
    await mkdir(path.dirname(binary), { recursive: true });
    await writeFile(binary, "prebuilt-test-binary", "utf8");
    if (process.platform !== "win32") await chmod(binary, 0o755);
    const env = { CARGO_EXE: "cargo-custom", DEVBOX_MCP_IMPLEMENTATION: "rust" };
    const spec = await prepareMcpImplementation(root, {
      env,
      runProcess: async (file, args, options) => {
        calls.push({ file, args, options });
        return { stdout: "", stderr: "", exitCode: 0 };
      },
    });

    assert.equal(resolveCargoCommand(env), "cargo-custom");
    assert.equal(spec.implementation, "rust");
    assert.equal(spec.file, binary);
    assert.equal(calls.length, 2);
    assert.equal(calls[0].file, "cargo-custom");
    assert.deepEqual(calls[0].args, [
      "build",
      "--manifest-path",
      path.join(root, "rust-mcp", "Cargo.toml"),
      "--target-dir",
      getRustTargetDir(root),
      "--release",
      "--locked",
    ]);
    assert.equal(calls[1].file, binary);
    assert.deepEqual(calls[1].args, ["--parity-report"]);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("Rust build failure leaves a rollback instruction before replacement", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-rust-cutover-fail-"));
  try {
    await assert.rejects(
      prepareMcpImplementation(root, {
        env: { DEVBOX_MCP_IMPLEMENTATION: "rust" },
        runProcess: async () => { throw new Error("cargo unavailable"); },
      }),
      (error) => {
        assert.match(error.message, /before the existing MCP was stopped/u);
        assert.match(error.message, /DEVBOX_MCP_IMPLEMENTATION=js/u);
        return true;
      },
    );
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});



test("checked preflight terminates stalled processes at its deadline", async () => {
  await assert.rejects(
    runCheckedProcess(process.execPath, ["-e", "setTimeout(() => {}, 30000)"], {
      label: "deadline-test",
      timeoutMs: 50,
    }),
    /deadline-test timed out after 50 ms/u,
  );
});

test("checked preflight timeout terminates spawned descendants", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-preflight-tree-"));
  const pidFile = path.join(root, "descendant.pid");
  const script = [
    "const fs=require('fs');",
    "const {spawn}=require('child_process');",
    "const child=spawn(process.execPath,['-e','setInterval(()=>{},1000)'],{stdio:'ignore'});",
    "fs.writeFileSync(process.argv[1],String(child.pid));",
    "setInterval(()=>{},1000);",
  ].join("");
  try {
    await assert.rejects(
      runCheckedProcess(process.execPath, ["-e", script, pidFile], {
        label: "tree-deadline-test",
        timeoutMs: 500,
      }),
      /tree-deadline-test timed out/u,
    );
    const descendantPid = Number.parseInt((await readFile(pidFile, "utf8")).trim(), 10);
    assert.ok(Number.isInteger(descendantPid) && descendantPid > 0);
    let alive = true;
    for (let attempt = 0; attempt < 20; attempt += 1) {
      try {
        process.kill(descendantPid, 0);
      } catch {
        alive = false;
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    assert.equal(alive, false, `descendant PID ${descendantPid} survived preflight timeout`);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

test("JS rollback preflight does not invoke Cargo", async () => {
  let calls = 0;
  const spec = await prepareMcpImplementation(path.resolve("/tmp/js-rollback"), {
    env: { DEVBOX_MCP_IMPLEMENTATION: "js" },
    implementation: "js",
    runProcess: async () => { calls += 1; },
  });
  assert.equal(spec.implementation, "js");
  assert.equal(calls, 0);
});
