import test from "node:test";
import assert from "node:assert/strict";

import {
  buildHostShellArgs,
  detectPlatform,
  defaultHostProgramAllowlist,
  resolveRuntimeMode,
  resolveHostShell,
} from "../src/platform.js";

test("detectPlatform recognizes Termux from PREFIX", () => {
  const platform = detectPlatform({ PREFIX: "/data/data/com.termux/files/usr" }, "linux");

  assert.equal(platform.id, "termux");
  assert.equal(platform.isTermux, true);
  assert.equal(platform.isLinux, true);
  assert.equal(platform.displayName, "Termux");
});

test("resolveRuntimeMode defaults to host on Termux and docker on Windows", () => {
  assert.equal(resolveRuntimeMode({ requestedMode: "", platform: detectPlatform({ PREFIX: "/data/data/com.termux/files/usr" }, "linux") }), "host");
  assert.equal(resolveRuntimeMode({ requestedMode: "", platform: detectPlatform({}, "win32") }), "docker");
  assert.equal(resolveRuntimeMode({ requestedMode: "docker", platform: detectPlatform({}, "darwin") }), "docker");
  assert.equal(resolveRuntimeMode({ requestedMode: "host", platform: detectPlatform({}, "linux") }), "host");
});

test("defaultHostProgramAllowlist includes shell-friendly tools on posix hosts", () => {
  const allowlist = defaultHostProgramAllowlist(detectPlatform({}, "linux"));

  assert.equal(allowlist.includes("bash"), true);
  assert.equal(allowlist.includes("git"), true);
  assert.equal(allowlist.includes("python3"), true);
  assert.equal(allowlist.includes("powershell"), false);
});

test("resolveHostShell prefers SHELL on posix and PowerShell on Windows", () => {
  assert.equal(resolveHostShell({ SHELL: "/bin/bash" }, detectPlatform({}, "linux")), "/bin/bash");
  assert.equal(resolveHostShell({}, detectPlatform({}, "linux")), "/bin/sh");
  assert.equal(resolveHostShell({}, detectPlatform({}, "win32")), "powershell.exe");
  assert.equal(resolveHostShell({ POWERSHELL_EXE: "C:\\Program Files\\PowerShell\\7\\pwsh.exe" }, detectPlatform({}, "win32")), "C:\\Program Files\\PowerShell\\7\\pwsh.exe");
});

test("Termux detection is a Linux subtype, never Windows", () => {
  const platform = detectPlatform({ TERMUX_VERSION: "0.119" }, "linux");
  assert.equal(platform.id, "termux");
  assert.equal(platform.isTermux, true);
  assert.equal(platform.isLinux, true);
  assert.equal(platform.isWindows, false);
});

test("buildHostShellArgs supports PowerShell, cmd, and POSIX shells", () => {
  assert.deepEqual(
    buildHostShellArgs("powershell.exe", "Write-Output ok", detectPlatform({}, "win32")).slice(-2),
    ["-Command", "Write-Output ok"],
  );
  assert.deepEqual(buildHostShellArgs("cmd.exe", "echo ok", detectPlatform({}, "win32")), ["/d", "/s", "/c", "echo ok"]);
  assert.deepEqual(buildHostShellArgs("/bin/bash", "printf ok", detectPlatform({}, "linux")), ["-lc", "printf ok"]);
});
