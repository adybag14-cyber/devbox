import test from "node:test";
import assert from "node:assert/strict";

import { spawnProcess } from "../src/process-utils.js";

process.env.MCP_AUTH_MODE = "none";
process.env.PUBLIC_BASE_URL = "";

const {
  buildWindowsPowerShellArgs,
  buildElevatedWindowsPowerShellWrapper,
  buildElevatedWindowsPowerShellLauncher,
  resolveHostProgramExecutable,
  getHostToolStatus,
} = await import("../src/host-tools.js");

test("buildWindowsPowerShellArgs encodes the original script exactly", () => {
  const command = "$value = 'A \"quoted\" value with ''single'' quotes'; Write-Output $value";
  const args = buildWindowsPowerShellArgs(command);

  assert.equal(args[0], "-NoLogo");
  assert.equal(args[1], "-NoProfile");
  assert.equal(args[2], "-NonInteractive");
  assert.equal(args[5], "-EncodedCommand");
  assert.equal(Buffer.from(args[6], "base64").toString("utf16le"), command);
});

test("encoded PowerShell execution preserves nested quotes end to end", async () => {
  const command = "$value = 'A \"quoted\" value with ''single'' quotes'; Write-Output $value";
  const result = await spawnProcess("powershell.exe", buildWindowsPowerShellArgs(command), {
    cwd: process.cwd(),
    timeoutMs: 15000,
  });

  assert.equal(result.stdout.trim(), "A \"quoted\" value with 'single' quotes");
});

test("resolveHostProgramExecutable maps node to the real node.exe path", () => {
  assert.equal(resolveHostProgramExecutable("node"), process.execPath);
  assert.equal(resolveHostProgramExecutable("node.exe"), process.execPath);
  assert.match(getHostToolStatus().resolvedNodeExe, /node(\.exe)?$/i);
});

test("buildElevatedWindowsPowerShellWrapper preserves working directory and output paths", () => {
  const wrapper = buildElevatedWindowsPowerShellWrapper({
    command: "Write-Output 'ok'",
    workingDir: "C:\\Temp\\quoted path",
    stdoutPath: "C:\\Temp\\stdout.txt",
    stderrPath: "C:\\Temp\\stderr.txt",
    exitCodePath: "C:\\Temp\\exit.txt",
  });

  assert.equal(wrapper.includes("Set-Location -LiteralPath 'C:\\Temp\\quoted path'"), true);
  assert.equal(wrapper.includes("$stdoutPath = 'C:\\Temp\\stdout.txt'"), true);
  assert.equal(wrapper.includes("$stderrPath = 'C:\\Temp\\stderr.txt'"), true);
  assert.equal(wrapper.includes("$exitCodePath = 'C:\\Temp\\exit.txt'"), true);
  assert.equal(wrapper.includes("$commandBase64 = '"), true);
});

test("buildElevatedWindowsPowerShellLauncher uses RunAs elevation", () => {
  const launcher = buildElevatedWindowsPowerShellLauncher({
    command: "Write-Output 'ok'",
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
