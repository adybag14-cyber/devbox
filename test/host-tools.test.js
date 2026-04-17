import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, rm, writeFile } from "node:fs/promises";

import { spawnProcess } from "../src/process-utils.js";

process.env.MCP_AUTH_MODE = "none";
process.env.PUBLIC_BASE_URL = "";

const {
  HostCommandError,
  MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE,
  buildWindowsPowerShellArgs,
  buildWindowsPowerShellFileArgs,
  buildElevatedWindowsPowerShellWrapper,
  buildElevatedWindowsPowerShellLauncher,
  inspectWindowsFile,
  readLargeFileOnHost,
  shouldUsePowerShellScriptFile,
  resolveHostProgramExecutable,
  getHostToolStatus,
  runAllowedProgram,
  writeLargeFileOnHost,
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

test("shouldUsePowerShellScriptFile switches to file-backed execution for large commands", () => {
  const smallCommand = "Write-Output 'ok'";
  const largeCommand = `Write-Output '${"x".repeat(90000)}'`;

  assert.equal(shouldUsePowerShellScriptFile(smallCommand), false);
  assert.equal(shouldUsePowerShellScriptFile(largeCommand), true);
  assert.ok(buildWindowsPowerShellArgs(largeCommand).join(" ").length >= MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE);
});

test("file-backed PowerShell execution handles very large one-shot payloads", async () => {
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

test("resolveHostProgramExecutable maps node to the real node.exe path", () => {
  assert.equal(resolveHostProgramExecutable("node"), process.execPath);
  assert.equal(resolveHostProgramExecutable("node.exe"), process.execPath);
  assert.match(getHostToolStatus().resolvedNodeExe, /node(\.exe)?$/i);
});

test("buildElevatedWindowsPowerShellWrapper preserves working directory and output paths", () => {
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

test("buildElevatedWindowsPowerShellLauncher uses RunAs elevation", () => {
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

test("inspectWindowsFile flags mojibake-corrupted PowerShell scripts and reports repair hints", async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-host-tools-test-"));
  const scriptPath = path.join(tempDir, "corrupt.ps1");
  const corruptedScript = `Write-Host 'ok'\n${"\u00e2\u20ac\u201d"}\nif ($true {\n`;

  try {
    await writeFile(scriptPath, corruptedScript, "utf8");
    const inspection = await inspectWindowsFile({
      path: scriptPath,
      workingDir: tempDir,
    });

    assert.equal(inspection.exists, true);
    assert.equal(inspection.is_file, true);
    assert.equal(inspection.extension, ".ps1");
    assert.equal(inspection.likely_corrupted_on_disk, true);
    assert.ok(inspection.suspicious_mojibake_count > 0);
    assert.equal(inspection.syntax_invalid, true);
    assert.equal(inspection.powershell_syntax?.parse_ok, false);
    assert.match(inspection.repair_hints.join("\n"), /windows_host_read_large_file/);
    assert.match(inspection.repair_hints.join("\n"), /windows_host_write_large_file/);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("runAllowedProgram attaches bridge diagnostics for corrupted script failures", async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-host-tools-test-"));
  const scriptPath = path.join(tempDir, "corrupt.ps1");
  const corruptedScript = `Write-Host 'ok'\n${"\u00e2\u20ac\u201d"}\nif ($true {\n`;

  try {
    await writeFile(scriptPath, corruptedScript, "utf8");

    await assert.rejects(
      () =>
        runAllowedProgram({
          program: "powershell.exe",
          args: ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", scriptPath],
          workingDir: tempDir,
          timeoutMs: 15000,
        }),
      (error) => {
        assert.ok(error instanceof HostCommandError);
        assert.ok(error.data?.bridge_diagnostics);
        assert.equal(error.data.bridge_diagnostics.suspected_file_integrity_issue, true);
        assert.ok(Array.isArray(error.data.bridge_diagnostics.inspected_paths));

        const inspectedPath = error.data.bridge_diagnostics.inspected_paths.find(
          (entry) => entry.resolved_path === scriptPath,
        );
        assert.ok(inspectedPath);
        assert.equal(inspectedPath.likely_corrupted_on_disk, true);
        assert.match(error.data.bridge_diagnostics.hints.join("\n"), /windows_host_write_large_file/);
        return true;
      },
    );
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});

test("host large-file helpers preserve exact bytes for repair workflows", async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-host-tools-test-"));
  const targetPath = path.join(tempDir, "exact-bytes.bin");
  const payload = Buffer.from([0x00, 0x41, 0xff, 0x42, 0x80, 0x43, 0x0d, 0x0a]);

  try {
    const writeResult = await writeLargeFileOnHost({
      path: targetPath,
      workingDir: tempDir,
      contentBase64: payload.toString("base64"),
    });
    const readResult = await readLargeFileOnHost({
      path: targetPath,
      workingDir: tempDir,
      offsetBytes: 0,
      maxBytes: payload.length,
    });

    assert.equal(writeResult.bytes_written, payload.length);
    assert.equal(readResult.bytes_returned, payload.length);
    assert.equal(readResult.content_base64, payload.toString("base64"));
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});
