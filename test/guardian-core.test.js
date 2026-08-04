import test from "node:test";
import assert from "node:assert/strict";

import {
  calculateRepairBackoffSeconds,
  classifyReadiness,
  isRepairAllowed,
  resolveSelectedRuntime,
  selectRepairScope,
  updateDockerRepairPolicy,
} from "../src/guardian-core.js";

test("host mode is healthy without probing or requiring Docker", () => {
  const state = classifyReadiness({
    selectedRuntime: "host",
    mcpProcessRunning: true,
    localHealth: true,
    publicEnabled: true,
    publicHealth: true,
    tunnelRunning: true,
    dockerReady: null,
    devboxContainerRunning: null,
  });

  assert.equal(state.IsHealthy, true);
  assert.equal(state.McpHealthy, true);
  assert.equal(state.PublicTunnelHealthy, true);
  assert.equal(state.SelectedRuntimeHealthy, true);
  assert.deepEqual(state.Reasons, []);
  assert.deepEqual(state.Readiness.Summary, [
    "MCP healthy",
    "public tunnel healthy",
    "selected runtime healthy",
    "optional components healthy",
  ]);
});

test("docker mode requires both the engine and selected container", () => {
  const state = classifyReadiness({
    selectedRuntime: "docker",
    mcpProcessRunning: true,
    localHealth: true,
    dockerReady: false,
  });

  assert.equal(state.IsHealthy, false);
  assert.equal(state.SelectedRuntimeHealthy, false);
  assert.deepEqual(state.Reasons, ["docker engine not ready"]);
});

test("an intentional stop is healthy-idle and never needs repair", () => {
  const state = classifyReadiness({
    shouldRun: false,
    selectedRuntime: "docker",
    mcpProcessRunning: false,
    localHealth: false,
    publicEnabled: true,
    publicHealth: false,
    tunnelRunning: false,
  });

  assert.equal(state.IsHealthy, true);
  assert.equal(state.NeedsRepair, false);
  assert.deepEqual(state.Reasons, []);
  assert.equal(state.Readiness.Overall, "stopped");
  assert.equal(state.Readiness.PublicTunnel, "stopped");
});

test("auto preserves a persisted selection and migrates a healthy legacy host", () => {
  assert.equal(resolveSelectedRuntime({ runtimeMode: "auto", selectedRuntime: "host", platform: "win32" }), "host");
  assert.equal(resolveSelectedRuntime({ runtimeMode: "auto", legacyHostHealthy: true, platform: "win32" }), "host");
  assert.equal(resolveSelectedRuntime({ runtimeMode: "auto", platform: "win32" }), "docker");
  assert.equal(resolveSelectedRuntime({ runtimeMode: "auto", platform: "linux" }), "host");
});

test("Docker repair failures back off exponentially and open a circuit", () => {
  const nowMs = Date.parse("2026-07-18T12:00:00.000Z");
  let policy = {};

  policy = updateDockerRepairPolicy({ policy, succeeded: false, nowMs });
  assert.equal(policy.ConsecutiveDockerFailures, 1);
  assert.equal(calculateRepairBackoffSeconds({ consecutiveFailures: 1 }), 60);
  assert.equal(policy.CircuitOpenUntilUtc, null);
  assert.equal(isRepairAllowed({ policy, nowMs: nowMs + 59_000 }), false);

  policy = updateDockerRepairPolicy({ policy, succeeded: false, nowMs: nowMs + 60_000 });
  assert.equal(policy.ConsecutiveDockerFailures, 2);
  assert.equal(calculateRepairBackoffSeconds({ consecutiveFailures: 2 }), 120);

  policy = updateDockerRepairPolicy({ policy, succeeded: false, nowMs: nowMs + 180_000 });
  assert.equal(policy.ConsecutiveDockerFailures, 3);
  assert.equal(policy.LastOutcome, "circuit-open");
  assert.equal(policy.CircuitOpenUntilUtc, "2026-07-18T13:03:00.000Z");
  assert.equal(isRepairAllowed({ policy, nowMs: nowMs + 3_779_000 }), false);
  assert.equal(isRepairAllowed({ policy, nowMs: nowMs + 3_781_000 }), true);

  policy = updateDockerRepairPolicy({ policy, succeeded: true, nowMs: nowMs + 3_781_000 });
  assert.equal(policy.ConsecutiveDockerFailures, 0);
  assert.equal(policy.CircuitOpenUntilUtc, null);
  assert.equal(isRepairAllowed({ policy, nowMs }), true);
});

test("a public-only failure selects a tunnel repair without restarting MCP", () => {
  assert.equal(selectRepairScope({
    Settings: { Public: true },
    McpHealthy: true,
    SelectedRuntimeHealthy: true,
    PublicTunnelHealthy: false,
  }), "public-tunnel");

  assert.equal(selectRepairScope({
    Settings: { Public: true },
    McpHealthy: false,
    SelectedRuntimeHealthy: false,
    PublicTunnelHealthy: false,
  }), "full");
});
