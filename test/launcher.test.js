import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import os from "node:os";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";

import { buildServerUrl, getLauncherPaths, getServerStatus, parseLauncherArgs, stopServerProcess, waitForServerReady } from "../src/launcher.js";

test("parseLauncherArgs defaults to background start and supports explicit commands", () => {
  assert.deepEqual(parseLauncherArgs([]), { command: "start", background: true });
  assert.deepEqual(parseLauncherArgs(["start"]), { command: "start", background: true });
  assert.deepEqual(parseLauncherArgs(["run"]), { command: "run", background: false });
  assert.deepEqual(parseLauncherArgs(["status"]), { command: "status", background: false });
});

test("getLauncherPaths stores pid and log files under run/", () => {
  const paths = getLauncherPaths("/tmp/devbox-project");

  assert.equal(paths.runDir, path.join("/tmp/devbox-project", "run"));
  assert.equal(paths.pidFile, path.join("/tmp/devbox-project", "run", "devbox.pid"));
  assert.equal(paths.logFile, path.join("/tmp/devbox-project", "run", "devbox.log"));
  assert.equal(paths.implementationFile, path.join("/tmp/devbox-project", "run", "devbox.implementation"));
  assert.equal(paths.managedPidFile, path.join("/tmp/devbox-project", "run", "mcp.pid"));
  assert.equal(paths.managedImplementationFile, path.join("/tmp/devbox-project", "run", "mcp.implementation"));
  assert.equal(paths.managedStdoutLogFile, path.join("/tmp/devbox-project", "run", "mcp.stdout.log"));
  assert.equal(paths.guardianDesiredStateFile, path.join("/tmp/devbox-project", "run", "guardian.desired-state.json"));
});

test("buildServerUrl normalizes wildcard hosts to loopback", () => {
  assert.equal(buildServerUrl({ host: "0.0.0.0", port: 8100 }), "http://127.0.0.1:8100");
  assert.equal(buildServerUrl({ host: "::", port: 8100 }), "http://127.0.0.1:8100");
  assert.equal(buildServerUrl({ host: "localhost", port: 8100 }), "http://localhost:8100");
});

test("waitForServerReady waits until the health endpoint is actually ready", async () => {
  let ready = false;
  const server = createServer((_request, response) => {
    response.statusCode = ready ? 200 : 503;
    response.end(ready ? "ok" : "starting");
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  assert.equal(typeof address, "object");
  setTimeout(() => { ready = true; }, 80);

  try {
    const result = await waitForServerReady({
      url: `http://127.0.0.1:${address.port}`,
      timeoutMs: 2000,
      pollIntervalMs: 20,
    });
    assert.equal(result.status, 200);
    assert.match(result.healthUrl, /\/healthz$/u);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("status is read-only while stop records intentional-stop state", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-launcher-"));
  try {
    const paths = getLauncherPaths(root);
    await getServerStatus(root);
    await assert.rejects(readFile(paths.guardianDesiredStateFile, "utf8"), { code: "ENOENT" });

    await stopServerProcess(root);
    const desired = JSON.parse(await readFile(paths.guardianDesiredStateFile, "utf8"));
    assert.equal(desired.ShouldRun, false);
    assert.equal(desired.Source, "devbox stop");
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});


test("status recognizes a healthy externally managed MCP without claiming stop ownership", async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-managed-status-"));
  const server = createServer((_request, response) => {
    response.statusCode = 200;
    response.end("ok");
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  assert.equal(typeof address, "object");
  const url = `http://127.0.0.1:${address.port}`;

  try {
    const paths = getLauncherPaths(root);
    await mkdir(paths.runDir, { recursive: true });
    await writeFile(paths.managedPidFile, `${process.pid}\n`, "utf8");
    await writeFile(paths.managedImplementationFile, "rust\n", "utf8");

    const status = await getServerStatus(root, { url, healthTimeoutMs: 1000 });
    assert.equal(status.running, true);
    assert.equal(status.healthy, true);
    assert.equal(status.pid, process.pid);
    assert.equal(status.manager, "managed-mcp");
    assert.equal(status.managedExternally, true);
    assert.equal(status.implementation, "rust");
    assert.equal(status.pidFile, paths.managedPidFile);

    const stop = await stopServerProcess(root, { url, healthTimeoutMs: 1000 });
    assert.equal(stop.stopRefused, true);
    assert.equal(stop.running, true);
    assert.match(stop.note, /managed MCP lifecycle/i);
    await assert.rejects(readFile(paths.guardianDesiredStateFile, "utf8"), { code: "ENOENT" });
  } finally {
    await new Promise((resolve) => server.close(resolve));
    await rm(root, { recursive: true, force: true });
  }
});
