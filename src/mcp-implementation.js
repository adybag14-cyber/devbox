import path from "node:path";
import { access } from "node:fs/promises";
import { constants as fsConstants } from "node:fs";
import { spawn } from "node:child_process";

const VALID_IMPLEMENTATIONS = new Set(["rust", "js"]);
const MAX_PREFLIGHT_OUTPUT_CHARS = 12000;

export const resolveMcpImplementation = (env = process.env) => {
  const value = String(env.DEVBOX_MCP_IMPLEMENTATION ?? "rust").trim().toLowerCase() || "rust";
  if (!VALID_IMPLEMENTATIONS.has(value)) {
    throw new Error(`Invalid DEVBOX_MCP_IMPLEMENTATION=${JSON.stringify(value)}; expected rust or js.`);
  }
  return value;
};

export const getRustMcpBinaryPath = (root, platform = process.platform) =>
  path.join(root, "rust-mcp", "target", "release", platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp");

export const getRustManifestPath = (root) => path.join(root, "rust-mcp", "Cargo.toml");

export const resolveCargoCommand = (env = process.env) => String(env.CARGO_EXE ?? "").trim() || "cargo";

const appendBounded = (existing, chunk) => `${existing}${chunk}`.slice(-MAX_PREFLIGHT_OUTPUT_CHARS);

export const runCheckedProcess = (file, args, {
  cwd,
  env = process.env,
  label = file,
  stdio = ["ignore", "pipe", "pipe"],
} = {}) => new Promise((resolve, reject) => {
  let stdout = "";
  let stderr = "";
  let child;
  try {
    child = spawn(file, args, { cwd, env, stdio, windowsHide: true });
  } catch (error) {
    reject(new Error(`${label} could not start: ${error instanceof Error ? error.message : String(error)}`));
    return;
  }
  child.stdout?.setEncoding("utf8");
  child.stderr?.setEncoding("utf8");
  child.stdout?.on("data", (chunk) => { stdout = appendBounded(stdout, chunk); });
  child.stderr?.on("data", (chunk) => { stderr = appendBounded(stderr, chunk); });
  child.once("error", (error) => {
    reject(new Error(`${label} could not start: ${error.message}`));
  });
  child.once("exit", (code, signal) => {
    if (code === 0) {
      resolve({ stdout, stderr, exitCode: 0 });
      return;
    }
    const detail = [stderr.trim(), stdout.trim()].filter(Boolean).join("\n");
    const suffix = detail ? `\n${detail}` : "";
    reject(new Error(`${label} failed (code=${code ?? "null"}, signal=${signal ?? "null"}).${suffix}`));
  });
});

export const getMcpLaunchSpec = (root, {
  env = process.env,
  platform = process.platform,
  implementation = resolveMcpImplementation(env),
} = {}) => {
  if (implementation === "js") {
    return {
      implementation,
      file: process.execPath,
      args: [path.join(root, "src", "server.js")],
      env,
    };
  }
  return {
    implementation,
    file: getRustMcpBinaryPath(root, platform),
    args: [],
    env: {
      ...env,
      DEVBOX_PROJECT_ROOT: root,
      DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE: "1",
    },
  };
};

export const prepareMcpImplementation = async (root, {
  env = process.env,
  platform = process.platform,
  implementation = resolveMcpImplementation(env),
  runProcess = runCheckedProcess,
} = {}) => {
  if (implementation === "js") {
    return getMcpLaunchSpec(root, { env, platform, implementation });
  }

  const manifest = getRustManifestPath(root);
  const cargo = resolveCargoCommand(env);
  try {
    await runProcess(cargo, ["build", "--manifest-path", manifest, "--release", "--locked"], {
      cwd: root,
      env,
      label: "Rust MCP release build",
    });
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    throw new Error(
      `Rust MCP preflight failed before the existing MCP was stopped. ${detail}\n` +
      "Install Rust/Cargo or set CARGO_EXE, then retry. Temporary rollback: DEVBOX_MCP_IMPLEMENTATION=js.",
    );
  }

  const spec = getMcpLaunchSpec(root, { env, platform, implementation });
  try {
    await access(spec.file, fsConstants.X_OK);
  } catch {
    throw new Error(`Rust MCP release build completed but ${spec.file} is not executable.`);
  }
  await runProcess(spec.file, ["--parity-report"], {
    cwd: root,
    env: spec.env,
    label: "Rust MCP binary preflight",
  });
  return spec;
};
