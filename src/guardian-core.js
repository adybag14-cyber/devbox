const RUNTIME_MODES = new Set(["auto", "host", "docker"]);

export const normalizeRuntimeMode = (value, fallback = "auto") => {
  const normalized = String(value ?? "").trim().toLowerCase();
  return RUNTIME_MODES.has(normalized) ? normalized : fallback;
};

export const resolveSelectedRuntime = ({
  runtimeMode = "auto",
  selectedRuntime,
  platform = process.platform,
  legacyHostHealthy = false,
} = {}) => {
  const requested = normalizeRuntimeMode(runtimeMode);
  if (requested === "host" || requested === "docker") {
    return requested;
  }

  const persisted = normalizeRuntimeMode(selectedRuntime, "");
  if (persisted === "host" || persisted === "docker") {
    return persisted;
  }

  if (legacyHostHealthy) {
    return "host";
  }

  return platform === "win32" ? "docker" : "host";
};

export const calculateRepairBackoffSeconds = ({
  consecutiveFailures = 0,
  baseSeconds = 60,
  maxSeconds = 1800,
} = {}) => {
  const failures = Math.max(0, Number.parseInt(consecutiveFailures, 10) || 0);
  if (failures === 0) {
    return 0;
  }

  return Math.min(maxSeconds, baseSeconds * 2 ** Math.max(0, failures - 1));
};

export const updateDockerRepairPolicy = ({
  policy = {},
  succeeded,
  nowMs = Date.now(),
  baseSeconds = 60,
  maxSeconds = 1800,
  circuitFailureThreshold = 3,
  circuitOpenSeconds = 3600,
} = {}) => {
  if (succeeded) {
    return {
      ConsecutiveDockerFailures: 0,
      CircuitOpenUntilUtc: null,
      NextRepairAtUtc: null,
      LastOutcome: "succeeded",
      LastUpdatedAtUtc: new Date(nowMs).toISOString(),
    };
  }

  const failures = Math.max(0, Number.parseInt(policy.ConsecutiveDockerFailures, 10) || 0) + 1;
  const backoffSeconds = calculateRepairBackoffSeconds({
    consecutiveFailures: failures,
    baseSeconds,
    maxSeconds,
  });
  const circuitOpen = failures >= circuitFailureThreshold;
  const delaySeconds = circuitOpen ? Math.max(backoffSeconds, circuitOpenSeconds) : backoffSeconds;
  const nextRepairAt = new Date(nowMs + delaySeconds * 1000).toISOString();

  return {
    ConsecutiveDockerFailures: failures,
    CircuitOpenUntilUtc: circuitOpen ? nextRepairAt : null,
    NextRepairAtUtc: nextRepairAt,
    LastOutcome: circuitOpen ? "circuit-open" : "failed",
    LastUpdatedAtUtc: new Date(nowMs).toISOString(),
  };
};

export const isRepairAllowed = ({ policy = {}, nowMs = Date.now() } = {}) => {
  const nextRepairMs = Date.parse(policy.NextRepairAtUtc ?? "");
  if (Number.isFinite(nextRepairMs) && nextRepairMs > nowMs) {
    return false;
  }

  const circuitMs = Date.parse(policy.CircuitOpenUntilUtc ?? "");
  return !(Number.isFinite(circuitMs) && circuitMs > nowMs);
};

export const selectRepairScope = (state = {}) => {
  const publicEnabled = state.Settings?.Public === true;
  const publicOnlyFailure =
    publicEnabled &&
    state.McpHealthy === true &&
    state.SelectedRuntimeHealthy === true &&
    state.PublicTunnelHealthy === false;

  return publicOnlyFailure ? "public-tunnel" : "full";
};

export const resolveFailureThreshold = ({ state = {}, configuredThreshold = 3 } = {}) => {
  const configured = Math.max(1, Number.parseInt(configuredThreshold, 10) || 1);
  // A failed localhost health probe means the MCP itself is unavailable or
  // hung, so recover after two observations. Public-only tunnel failures keep
  // the configured threshold to tolerate normal edge/QUIC reconnect churn.
  return state.LocalHealth === false ? Math.min(2, configured) : configured;
};

export const classifyReadiness = ({
  shouldRun = true,
  selectedRuntime = "host",
  mcpProcessRunning = false,
  localHealth = false,
  publicEnabled = false,
  publicHealth = null,
  tunnelRunning = null,
  dockerReady = null,
  devboxContainerRunning = null,
  optionalDegradations = [],
  // When true (Windows host mode), MCP must already be elevated so host_exec
  // never falls back to Start-Process -Verb RunAs (UAC spam).
  requireMcpElevated = false,
  mcpElevated = null,
} = {}) => {
  const runtime = selectedRuntime === "docker" ? "docker" : "host";
  const reasons = [];
  const degradations = [...new Set(optionalDegradations.filter(Boolean).map(String))];
  // Elevation probes are allowed to return null when Windows token inspection
  // itself fails. Unknown must not be treated as a definitive medium-integrity
  // token or Guardian can restart a healthy MCP because of a transient probe.
  const elevationKnownBad = requireMcpElevated && mcpElevated === false;
  const elevationOk = !elevationKnownBad;
  if (requireMcpElevated && mcpProcessRunning && mcpElevated === null) {
    degradations.push("MCP elevation could not be verified; retaining the current process");
  }
  const mcpHealthy = Boolean(mcpProcessRunning && localHealth && elevationOk);
  const publicTunnelHealthy = publicEnabled
    ? Boolean(publicHealth && tunnelRunning !== false)
    : null;
  const selectedRuntimeHealthy = runtime === "docker"
    ? Boolean(dockerReady && devboxContainerRunning)
    : mcpHealthy;

  if (shouldRun) {
    if (!mcpProcessRunning) {
      reasons.push("MCP server process is missing");
    }
    if (!localHealth) {
      reasons.push("local health check failed");
    }
    if (requireMcpElevated && mcpProcessRunning && mcpElevated === false) {
      reasons.push("MCP server process is not elevated (host PowerShell would trigger UAC)");
    }
    if (runtime === "docker" && !dockerReady) {
      reasons.push("docker engine not ready");
    } else if (runtime === "docker" && !devboxContainerRunning) {
      reasons.push("devbox container is not running");
    }
    if (publicEnabled && tunnelRunning === false) {
      reasons.push("public tunnel is not running");
    }
    if (publicEnabled && !publicHealth) {
      reasons.push("public health check failed");
    }
  }

  const healthy = !shouldRun || (mcpHealthy && selectedRuntimeHealthy && publicTunnelHealthy !== false);
  const overall = !shouldRun ? "stopped" : healthy ? "healthy" : "unhealthy";
  const readiness = {
    Overall: overall,
    Mcp: !shouldRun ? "stopped" : mcpHealthy ? "healthy" : "unhealthy",
    PublicTunnel: !shouldRun ? "stopped" : !publicEnabled ? "disabled" : publicTunnelHealthy ? "healthy" : "unhealthy",
    SelectedRuntime: !shouldRun ? "stopped" : selectedRuntimeHealthy ? "healthy" : "unhealthy",
    OptionalComponents: degradations.length > 0 ? "degraded" : "healthy",
    Summary: [
      `MCP ${!shouldRun ? "stopped" : mcpHealthy ? "healthy" : "unhealthy"}`,
      `public tunnel ${!shouldRun ? "stopped" : !publicEnabled ? "disabled" : publicTunnelHealthy ? "healthy" : "unhealthy"}`,
      `selected runtime ${!shouldRun ? "stopped" : selectedRuntimeHealthy ? "healthy" : "unhealthy"}`,
      `optional components ${degradations.length > 0 ? "degraded" : "healthy"}`,
    ],
  };

  return {
    IsHealthy: healthy,
    NeedsRepair: shouldRun && !healthy,
    McpHealthy: mcpHealthy,
    PublicTunnelHealthy: publicTunnelHealthy,
    SelectedRuntimeHealthy: selectedRuntimeHealthy,
    OptionalComponentsDegraded: degradations.length > 0,
    OptionalDegradations: degradations,
    Readiness: readiness,
    Reasons: reasons,
  };
};
