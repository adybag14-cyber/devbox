import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";

import { spawnProcess } from "../src/process-utils.js";
import { isJpegBuffer } from "../src/windows-screen-capture.js";

const hasPowerShell = process.platform === "win32";

const importFreshHostTools = async () => {
  process.env.MCP_AUTH_MODE = "none";
  process.env.PUBLIC_BASE_URL = "";
  const href = pathToFileURL(path.join(process.cwd(), "src/host-tools.js")).href;
  return import(`${href}?t=${Date.now()}-${Math.random()}`);
};
const {
  HostCommandError,
  MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE,
  buildWindowsPowerShellArgs,
  buildWindowsPowerShellFileArgs,
  buildElevatedWindowsPowerShellWrapper,
  buildElevatedWindowsPowerShellLauncher,
  cleanPowerShellOutput,
  inspectWindowsFile,
  readLargeFileOnHost,
  shouldUsePowerShellScriptFile,
  resolveHostProgramExecutable,
  getHostToolStatus,
  runAllowedProgram,
  writeLargeFileOnHost,
} = await importFreshHostTools();

test("isJpegBuffer accepts complete JPEG bytes and rejects truncated data", () => {
  assert.equal(isJpegBuffer(Buffer.from([0xff, 0xd8, 0xff, 0x01, 0x02, 0xff, 0xd9])), true);
  assert.equal(isJpegBuffer(Buffer.from([0xff, 0xd8, 0xff, 0x01, 0x02])), false);
  assert.equal(isJpegBuffer(Buffer.from("not-a-jpeg")), false);
});

test("an already-cancelled host PowerShell call does not wait for the shared elevation probe", { skip: !hasPowerShell }, async () => {
  const { runWindowsPowerShell } = await importFreshHostTools();
  const controller = new AbortController();
  controller.abort();
  const startedAt = Date.now();
  await assert.rejects(
    runWindowsPowerShell({ command: "Write-Output should-not-run", workingDir: process.cwd(), timeoutMs: 15000, signal: controller.signal }),
    /cancelled by the MCP client/u,
  );
  assert.ok(Date.now() - startedAt < 1000);
});

test("buildWindowsPowerShellArgs suppresses progress streams before the original script", async () => {
  const { buildWindowsPowerShellArgs } = await importFreshHostTools();
  const command = "$value = 'A \"quoted\" value with ''single'' quotes'; Write-Output $value";
  const args = buildWindowsPowerShellArgs(command);

  assert.equal(args[0], "-NoLogo");
  assert.equal(args[1], "-NoProfile");
  assert.equal(args[2], "-NonInteractive");
  assert.equal(args[5], "-EncodedCommand");
  const decoded = Buffer.from(args[6], "base64").toString("utf16le");
  assert.match(decoded, /^\$ProgressPreference = 'SilentlyContinue'\n\$InformationPreference = 'SilentlyContinue'/u);
  assert.equal(decoded.endsWith(command), true);
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
  assert.equal(wrapper.includes("$InformationPreference = 'SilentlyContinue'"), true);
});

test("buildElevatedWindowsPowerShellLauncher uses the configured PowerShell executable for RunAs elevation", async () => {
  const { buildElevatedWindowsPowerShellLauncher } = await importFreshHostTools();
  const launcher = buildElevatedWindowsPowerShellLauncher({
    scriptPath: "C:\\Temp\\command.ps1",
    workingDir: "C:\\Temp",
    stdoutPath: "C:\\Temp\\stdout.txt",
    stderrPath: "C:\\Temp\\stderr.txt",
    exitCodePath: "C:\\Temp\\exit.txt",
    timeoutMs: 15000,
    powerShellExe: "C:\\Program Files\\PowerShell\\7\\pwsh.exe",
  });

  assert.match(launcher, /Start-Process -FilePath 'C:\\Program Files\\PowerShell\\7\\pwsh\.exe' -Verb RunAs/);
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

test("PowerShell CLIXML progress is removed while serialized errors are decoded", () => {
  const progressOnly = "#< CLIXML\r\n<Objs Version=\"1.1.0.1\"><Obj S=\"progress\"><MS><S N=\"Activity\">Working</S></MS></Obj></Objs>";
  const serializedError = "#< CLIXML\r\n<Objs Version=\"1.1.0.1\"><S S=\"Error\">real failure_x000D__x000A_</S></Objs>";

  assert.equal(cleanPowerShellOutput(progressOnly), "");
  assert.equal(cleanPowerShellOutput(serializedError).trim(), "real failure");
});

test("inspectWindowsFile skips corruption checks for PE executables", async () => {
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-host-tools-test-"));
  const executablePath = path.join(tempDir, "valid.exe");

  try {
    await writeFile(executablePath, Buffer.from([0x4d, 0x5a, 0x00, 0x00, 0xff, 0xfe]));
    const inspection = await inspectWindowsFile({ path: executablePath, workingDir: tempDir });

    assert.equal(inspection.binary_format, "pe");
    assert.equal(inspection.text_inspection_skipped, true);
    assert.equal(inspection.likely_corrupted_on_disk, false);
    assert.equal(inspection.utf8_valid, null);
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

test("automatic diagnostics ignore an existing node.exe binary mentioned by a failed command", async () => {
  await assert.rejects(
    () =>
      runAllowedProgram({
        program: "node",
        args: ["-e", "console.error(JSON.stringify(process.execPath)); process.exit(3)"],
        workingDir: process.cwd(),
        timeoutMs: 15000,
      }),
    (error) => {
      assert.ok(error instanceof HostCommandError);
      assert.equal(error.exitCode, 3);
      assert.equal(error.data?.bridge_diagnostics, undefined);
      return true;
    },
  );
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

test("HostCommandError preserves timeout and cancellation metadata", () => {
  const error = new HostCommandError("timed out", { timedOut: true, aborted: true, signal: "SIGTERM" });
  assert.equal(error.timedOut, true);
  assert.equal(error.aborted, true);
  assert.equal(error.signal, "SIGTERM");
});
