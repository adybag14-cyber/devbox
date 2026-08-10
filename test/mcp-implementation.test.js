import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { chmod, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";

import {
  getMcpLaunchSpec,
  getRustMcpBinaryPath,
  prepareMcpImplementation,
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
