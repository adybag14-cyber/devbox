import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import os from "node:os";
import { mkdtemp, readFile, rm } from "node:fs/promises";
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
