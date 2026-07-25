import test from "node:test";
import assert from "node:assert/strict";

import {
  acquireGuardianLock,
  buildWindowsGuardianRepairArgs,
  classifyCommandFailure,
  ensureDockerContainer,
  isGuardianCommandLine,
  isGuardianLockOwner,
  isProcessAlive,
  runProcessUntilExit,
} from "../scripts/devbox-guardian.mjs";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

const environment = {
  DEVBOX_PROJECT_ROOT: "/repo",
  DEVBOX_IMAGE_NAME: "devbox:test",
  HOST_WORKSPACE_PATH: "/repo/workspace",
  DEVBOX_WORKSPACE_PATH: "/workspace",
};
const settings = { DevboxContainerName: "devbox-runtime" };
const result = (exitCode, stdout = "", stderr = "") => ({ exitCode, stdout, stderr });

test("Windows EPERM process probes still mean the PID is alive", () => {
  const deniedProbe = () => {
    const error = new Error("operation not permitted");
    error.code = "EPERM";
    throw error;
  };
  const missingProbe = () => {
    const error = new Error("no such process");
    error.code = "ESRCH";
    throw error;
  };

  assert.equal(isProcessAlive(9244, deniedProbe), true);
  assert.equal(isProcessAlive(9244, missingProbe), false);
  assert.equal(isProcessAlive(null, deniedProbe), false);
});

test("guardian lock ownership rejects a live PID reused by an unrelated process", async () => {
  const projectRoot = "C:\\repo";
  assert.equal(isGuardianCommandLine('"C:\\Program Files\\nodejs\\node.exe" C:\\repo\\scripts\\devbox-guardian.mjs', projectRoot), true);
  assert.equal(isGuardianCommandLine("C:\\Windows\\system32\\conhost.exe 0x4", projectRoot), false);

  assert.equal(await isGuardianLockOwner(18108, projectRoot, {
    processAlive: () => true,
    commandLineReader: async () => "C:\\Windows\\system32\\conhost.exe 0x4",
  }), false);
});

test("guardian lock ownership corroborates hidden Windows command lines", async () => {
  const nowMs = Date.parse("2026-07-29T11:16:00.000Z");
  const common = {
    processAlive: () => true,
    commandLineReader: async () => "",
    nowMs,
  };

  assert.equal(await isGuardianLockOwner(9244, "C:\\repo", {
    ...common,
    heartbeat: { SupervisorPid: 9244, ObservedAtUtc: "2026-07-29T11:15:58.000Z" },
  }), true);
  assert.equal(await isGuardianLockOwner(18108, "C:\\repo", {
    ...common,
    heartbeat: { SupervisorPid: 18108, ObservedAtUtc: "2026-07-29T00:15:38.000Z" },
    lockModifiedAtMs: Date.parse("2026-07-29T00:15:02.000Z"),
  }), false);
  assert.equal(await isGuardianLockOwner(777, "C:\\repo", {
    ...common,
    lockModifiedAtMs: nowMs - 1000,
  }), true);
});

test("guardian replaces a stale reused-PID lock but preserves a real owner lock", async () => {
  const directory = await mkdtemp(path.join(tmpdir(), "devbox-guardian-lock-"));
  const lockPath = path.join(directory, "guardian.lock");
  try {
    await writeFile(lockPath, "18108\n", "ascii");
    assert.equal(await acquireGuardianLock(lockPath, "C:\\repo", async () => false), true);
    assert.equal(Number.parseInt(await readFile(lockPath, "ascii"), 10), process.pid);

    assert.equal(await acquireGuardianLock(lockPath, "C:\\repo", async () => true), false);
    assert.equal(Number.parseInt(await readFile(lockPath, "ascii"), 10), process.pid);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test("stale container repair starts an existing stopped container without docker run", async () => {
  const calls = [];
  const responses = [result(0, "false\n"), result(0, "devbox-runtime\n"), result(0, "true\n")];
  const runner = async (_environment, args) => {
    calls.push(args);
    return responses.shift();
  };

  const repaired = await ensureDockerContainer(environment, settings, 5, runner);
  assert.equal(repaired.action, "started-existing");
  assert.deepEqual(calls.map((args) => args[0]), ["container", "start", "container"]);
  assert.equal(calls.some((args) => args[0] === "run"), false);
});

test("ambiguous Docker inspect errors never fall through to docker run", async () => {
  const calls = [];
  const runner = async (_environment, args) => {
    calls.push(args);
    return result(124, "", "Docker Desktop timed out");
  };

  await assert.rejects(
    ensureDockerContainer(environment, settings, 5, runner),
    /refusing a conflicting docker run/u,
  );
  assert.deepEqual(calls.map((args) => args[0]), ["container"]);
});

test("a stopped container that cannot start is removed and replaced", async () => {
  const calls = [];
  const responses = [
    result(0, "false\n"),
    result(1, "", "start failed"),
    result(0, "devbox-runtime\n"),
    result(0, "new-container-id\n"),
  ];
  const runner = async (_environment, args) => {
    calls.push(args);
    return responses.shift();
  };

  const repaired = await ensureDockerContainer(environment, settings, 5, runner);
  assert.equal(repaired.action, "created");
  assert.deepEqual(calls.map((args) => args[0]), ["container", "start", "rm", "run"]);
});

test("a create race re-inspects and starts the named container", async () => {
  const calls = [];
  const responses = [
    result(1, "", "Error: No such container: devbox-runtime"),
    result(125, "", "Conflict. The container name /devbox-runtime is already in use"),
    result(0, "false\n"),
    result(0, "devbox-runtime\n"),
  ];
  const runner = async (_environment, args) => {
    calls.push(args);
    return responses.shift();
  };

  const repaired = await ensureDockerContainer(environment, settings, 5, runner);
  assert.equal(repaired.action, "started-raced-existing");
  assert.deepEqual(calls.map((args) => args[0]), ["container", "run", "container", "start"]);
});

test("guardian classifies a killed timeout before an integer zero exit code", () => {
  const outcome = classifyCommandFailure(
    { killed: true, code: 0, signal: "SIGTERM" },
    { startedAtMs: 1000, nowMs: 154000 },
  );

  assert.deepEqual(outcome, {
    exitCode: 124,
    timedOut: true,
    signal: "SIGTERM",
    elapsedMs: 153000,
  });
});

test("guardian classifies a boundary-duration zero exit as timed out", () => {
  const outcome = classifyCommandFailure(
    { code: 0 },
    { startedAtMs: 1000, nowMs: 151000, timeoutMs: 150000 },
  );

  assert.equal(outcome.exitCode, 124);
  assert.equal(outcome.timedOut, true);
  assert.equal(outcome.elapsedMs, 150000);
});

test("repair runner resolves on parent exit even when a grandchild inherits its pipes", async () => {
  const startedAt = Date.now();
  const childScript = [
    "const { spawn } = require('node:child_process');",
    "const child = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 1500)'],",
    "  { stdio: ['ignore', 'inherit', 'inherit'] });",
    "child.unref();",
  ].join("\n");

  const result = await runProcessUntilExit(process.execPath, ["-e", childScript], {
    timeout: 1000,
    encoding: "utf8",
  });

  assert.equal(result.exitCode, 0);
  assert.ok(Date.now() - startedAt < 750);
});

test("repair runner rejects promptly when output exceeds its buffer", async () => {
  const startedAt = Date.now();
  const childScript = "process.stdout.write('x'.repeat(4096)); setInterval(() => {}, 1000);";

  await assert.rejects(
    runProcessUntilExit(process.execPath, ["-e", childScript], {
      timeout: 10000,
      maxBuffer: 128,
      encoding: "utf8",
    }),
    (error) => error?.code === "ENOBUFS" && error?.killed === true,
  );

  assert.ok(Date.now() - startedAt < 4000);
});

test("Windows public-only repair forces wrapper exit and selects TunnelOnly", () => {
  const args = buildWindowsGuardianRepairArgs({
    scriptPath: "C:\\repo\\scripts\\Start-ChatGptDevboxMcp.ps1",
    selectedRuntime: "host",
    settings: { Public: true, OAuth: true },
    repairScope: "public-tunnel",
  });
  const encodedIndex = args.indexOf("-EncodedCommand");
  const command = Buffer.from(args[encodedIndex + 1], "base64").toString("utf16le");

  assert.match(command, /-TunnelOnly/u);
  assert.match(command, /\[System\.Environment\]::Exit\(\$exitCode\)/u);
  assert.match(command, /\$ProgressPreference = 'SilentlyContinue'/u);
  assert.match(command, /\$InformationPreference = 'SilentlyContinue'/u);
});
