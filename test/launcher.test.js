import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";

import { buildServerUrl, getLauncherPaths, parseLauncherArgs } from "../src/launcher.js";

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
});

test("buildServerUrl normalizes wildcard hosts to loopback", () => {
  assert.equal(buildServerUrl({ host: "0.0.0.0", port: 8100 }), "http://127.0.0.1:8100");
  assert.equal(buildServerUrl({ host: "::", port: 8100 }), "http://127.0.0.1:8100");
  assert.equal(buildServerUrl({ host: "localhost", port: 8100 }), "http://localhost:8100");
});
