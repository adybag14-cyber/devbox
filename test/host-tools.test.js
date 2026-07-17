import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";

import { spawnProcess } from "../src/process-utils.js";

const hasPowerShell = process.platform === "win32";

const importFreshHostTools = async () => {
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-tools.js")).href;
  return import(`${href}?t=${Date.now()}-${Math.random()}`);
};

test("buildWindowsPowerShellArgs encodes the original script exactly", async () => {
  const { buildWindowsPowerShellArgs } = await importFreshHostTools();
  const command = "$value = 'A \"quoted\" value with ''single'' quotes'; Write-Output $value";
  const args = buildWindowsPowerShellArgs(command);

  assert.equal(args[0], "-NoLogo");
  assert.equal(args[1], "-NoProfile");
  assert.equal(args[2], "-NonInteractive");
  assert.equal(args[5], "-EncodedCommand");
  assert.equal(Buffer.from(args[6], "base64").toString("utf16le"), command);
});

test("encoded PowerShell execution preserves nested quotes end to end", { skip: !hasPowerShell }, async () => {
  const { buildWindowsPowerShellArgs } = await importFreshHostTools();
  const command = "$value = 'A \"quoted\" value with ''single'' quotes'; Write-Output $value";
  const result = await spawnProcess("powershell.exe", buildWindowsPowerShellArgs(command), {
    cwd: process.cwd(),
    timeoutMs: 15000,
  });

  assert.equal(result.stdout.trim(), "A \"quoted\" value with 'single' quotes");
});

test("shouldUsePowerShellScriptFile switches to file-backed execution for large commands", async () => {
  const { MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE, buildWindowsPowerShellArgs, shouldUsePowerShellScriptFile } = await importFreshHostTools();
  const smallCommand = "Write-Output 'ok'";
  const largeCommand = `Write-Output '${"x".repeat(90000)}'`;

  assert.equal(shouldUsePowerShellScriptFile(smallCommand), false);
  assert.equal(shouldUsePowerShellScriptFile(largeCommand), true);
  assert.ok(buildWindowsPowerShellArgs(largeCommand).join(" ").length >= MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE);
});

test("file-backed PowerShell execution handles very large one-shot payloads", { skip: !hasPowerShell }, async () => {
  const { buildWindowsPowerShellFileArgs } = await importFreshHostTools();
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-host-tools-test-"));
  const scriptPath = path.join(tempDir, "large-command.ps1");
  const payload = "x".repeat(90000);

  try {
    await writeFile(scriptPath, `Write-Output '${payload}'`, "utf8");
    const result = await spawnProcess("powershell.exe", buildWindowsPowerShellFileArgs(scriptPath), {
      cwd: tempDir,
      timeoutMs: 15000,
    });

    assert.equal(result.stdout.trim().length, payload.length);
    assert.equal(result.stdout.trim(), payload);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("resolveHostProgramExecutable maps node to the real node path", async () => {
  delete process.env.NODE_EXE;
  const { getHostToolStatus, resolveHostProgramExecutable } = await importFreshHostTools();
  assert.equal(resolveHostProgramExecutable("node"), process.execPath);
  assert.equal(resolveHostProgramExecutable("node.exe"), process.execPath);
  assert.match(getHostToolStatus().resolvedNodeExe, /node(\.exe)?$/i);
});

test("buildElevatedWindowsPowerShellWrapper preserves working directory and output paths", async () => {
  const { buildElevatedWindowsPowerShellWrapper } = await importFreshHostTools();
  const wrapper = buildElevatedWindowsPowerShellWrapper({
    scriptPath: "C:\\Temp\\command.ps1",
    workingDir: "C:\\Temp\\quoted path",
    stdoutPath: "C:\\Temp\\stdout.txt",
    stderrPath: "C:\\Temp\\stderr.txt",
    exitCodePath: "C:\\Temp\\exit.txt",
  });

  assert.equal(wrapper.includes("Set-Location -LiteralPath 'C:\\Temp\\quoted path'"), true);
  assert.equal(wrapper.includes("$stdoutPath = 'C:\\Temp\\stdout.txt'"), true);
  assert.equal(wrapper.includes("$stderrPath = 'C:\\Temp\\stderr.txt'"), true);
  assert.equal(wrapper.includes("$exitCodePath = 'C:\\Temp\\exit.txt'"), true);
  assert.equal(wrapper.includes("& 'C:\\Temp\\command.ps1'"), true);
});

test("buildElevatedWindowsPowerShellLauncher uses RunAs elevation", async () => {
  const { buildElevatedWindowsPowerShellLauncher } = await importFreshHostTools();
  const launcher = buildElevatedWindowsPowerShellLauncher({
    scriptPath: "C:\\Temp\\command.ps1",
    workingDir: "C:\\Temp",
    stdoutPath: "C:\\Temp\\stdout.txt",
    stderrPath: "C:\\Temp\\stderr.txt",
    exitCodePath: "C:\\Temp\\exit.txt",
    timeoutMs: 15000,
  });

  assert.match(launcher, /Start-Process -FilePath 'powershell\.exe' -Verb RunAs/);
  assert.match(launcher, /WaitForExit\(15000\)/);
  assert.match(launcher, /Stop-Process -Id \$process\.Id -Force/);
});

test("runHostShellCommand executes a posix shell command on non-Windows hosts", { skip: hasPowerShell }, async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "devbox-host-shell-"));
  process.env.HOST_DEFAULT_WORKDIR = tempDir;
  process.env.HOST_SHELL = "/bin/sh";
  process.env.ENABLE_HOST_EXEC = "true";

  const { runHostShellCommand } = await importFreshHostTools();
  const result = await runHostShellCommand({ command: "printf 'host-shell-ok'", workingDir: tempDir, timeoutMs: 5000 });

  assert.equal(result.stdout, "host-shell-ok");
});
