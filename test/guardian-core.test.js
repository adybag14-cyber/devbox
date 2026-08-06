import test from "node:test";
import assert from "node:assert/strict";

import {
  calculateRepairBackoffSeconds,
  classifyReadiness,
  isRepairAllowed,
  resolveSelectedRuntime,
  resolveFailureThreshold,
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

test("Windows host mode treats unelevated MCP as unhealthy to prevent UAC host_exec", () => {
  const healthyElevated = classifyReadiness({
    selectedRuntime: "host",
    mcpProcessRunning: true,
    localHealth: true,
    requireMcpElevated: true,
    mcpElevated: true,
  });
  assert.equal(healthyElevated.IsHealthy, true);
  assert.deepEqual(healthyElevated.Reasons, []);

  const unelevated = classifyReadiness({
    selectedRuntime: "host",
    mcpProcessRunning: true,
    localHealth: true,
    requireMcpElevated: true,
    mcpElevated: false,
  });
  assert.equal(unelevated.IsHealthy, false);
  assert.equal(unelevated.McpHealthy, false);
  assert.equal(unelevated.NeedsRepair, true);
  assert.match(unelevated.Reasons.join("; "), /not elevated/i);
});

test("Windows host mode keeps a healthy MCP when elevation inspection is temporarily unknown", () => {
  const unknown = classifyReadiness({
    selectedRuntime: "host",
    mcpProcessRunning: true,
    localHealth: true,
    requireMcpElevated: true,
    mcpElevated: null,
  });

  assert.equal(unknown.IsHealthy, true);
  assert.equal(unknown.McpHealthy, true);
  assert.equal(unknown.NeedsRepair, false);
  assert.deepEqual(unknown.Reasons, []);
  assert.match(unknown.OptionalDegradations.join("; "), /could not be verified/i);
});

test("local MCP health failures use a faster repair threshold than tunnel-only failures", () => {
  assert.equal(resolveFailureThreshold({ state: { LocalHealth: false }, configuredThreshold: 3 }), 2);
  assert.equal(resolveFailureThreshold({ state: { LocalHealth: false }, configuredThreshold: 1 }), 1);
  assert.equal(resolveFailureThreshold({ state: { LocalHealth: true }, configuredThreshold: 3 }), 3);
  assert.equal(resolveFailureThreshold({ state: { LocalHealth: null }, configuredThreshold: 4 }), 4);
});
