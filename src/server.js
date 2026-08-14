import { appendFile, mkdir, open, readFile, rename, rm, stat, statfs, writeFile } from "node:fs/promises";
import path from "node:path";
import { createHash, randomUUID } from "node:crypto";
import { fileURLToPath } from "node:url";
import { monitorEventLoopDelay } from "node:perf_hooks";

import express from "express";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { hostHeaderValidation, localhostHostValidation } from "@modelcontextprotocol/sdk/server/middleware/hostHeaderValidation.js";
import { mcpAuthRouter, getOAuthProtectedResourceMetadataUrl } from "@modelcontextprotocol/sdk/server/auth/router.js";
import { requireBearerAuth } from "@modelcontextprotocol/sdk/server/auth/middleware/bearerAuth.js";
import * as z from "zod/v4";

import { config, version } from "./config.js";
import {
  ensureDevboxRunning,
  execInDevbox,
  execReadOnlyInDevbox,
  getDevboxInfo,
  getDevboxGithubAuthStatus,
  getCachedDevboxVersions,
  getDevboxVersions,
  hostCommandTitle,
  hostTitle,
  isDockerRuntime,
  listFilesInDevbox,
  readFileInDevbox,
  readLargeFileInDevbox,
  recreateDevbox,
  restartDevbox,
  runProgramInDevbox,
  runtimeLabel,
  runtimeServerName,
  runtimeTitle,
  searchFilesInDevbox,
  syncGithubAuthToDevbox,
  stopDevbox,
  writeFileInDevbox,
  writeLargeFileInDevbox,
} from "./runtime.js";
import {
  HostCommandError,
  getHostGithubAuthContext,
  getHostToolStatus,
  inspectWindowsFile,
  readLargeFileOnHost,
  runAllowedProgram,
  runHostShellCommand,
  warmHostExecutionState,
  writeLargeFileOnHost,
} from "./host-tools.js";
import { captureHostDisplay, captureHostProgram } from "./screen-capture.js";
import { CloudflareAccessOAuthProvider, DemoOAuthProvider } from "./oauth.js";
import {
  normalizeLargeWritePayload,
  summarizeLargeReadData,
  summarizeLargeWriteData,
} from "./large-file-cli.js";
import { trimText } from "./process-utils.js";
import { shapeProcessOutput } from "./output-shaping.js";
import { abortableSleep, waitForPathCondition } from "./wait-utils.js";
import { getExecutionSlotSnapshot, probeExecutionSlotStoreWritable, withExecutionSlot } from "./execution-slots.js";
import { refreshExecutionStoreHealth as probeExecutionStoreHealth } from "./execution-store-health.js";
import {
  cancelDevboxJob,
  getDevboxJobLogs,
  getDevboxJobStatus,
  inferJobResourceClass,
  reconcileOrphanedDevboxJobs,
  startDevboxJob,
  startDevboxProgramJob,
  waitForDevboxJobStatus,
} from "./async-jobs.js";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const runDir = path.join(projectRoot, "run");
const jobsRoot = path.join(runDir, "jobs");
const jobsRootReady = mkdir(jobsRoot, { recursive: true });
const guardianDesiredStatePath = path.join(runDir, "guardian.desired-state.json");
const toolUsageLogPath = path.join(runDir, "tool-usage.jsonl");
const httpUsageLogPath = path.join(runDir, "http-usage.jsonl");
const usageLogStates = new Map();
const activeMcpRequestControllers = new Map();
const guardianStatePath = path.join(runDir, "guardian", "state.json");
const startupStatePath = path.join(runDir, "startup-state.json");
const mcpPerformanceStatePath = process.env.MCP_PERFORMANCE_STATE_PATH?.trim() ? path.resolve(process.env.MCP_PERFORMANCE_STATE_PATH.trim()) : path.join(runDir, "mcp-performance.json");
const EXECUTION_STORE_PROBE_HEALTHY_INTERVAL_MS = 60_000;
const EXECUTION_STORE_PROBE_RETRY_INTERVAL_MS = 10_000;
const EXECUTION_STORE_PROBE_STALE_MS = 150_000;
const EXECUTION_STORE_MIN_FREE_BYTES = 512 * 1024 * 1024;
const EXECUTION_STORE_WARN_FREE_BYTES = 50 * 1024 * 1024 * 1024;
const EXECUTION_STORE_WARN_FREE_PERCENT = 5;
let executionStoreHealth = { ok: false, sampledAtUtc: null, sampledAtMs: 0, error: "execution-store probe has not completed yet" };

const probeWritablePath = async (root, label) => {
  await mkdir(root, { recursive: true });
  const probePath = path.join(root, `.mcp-ready-${label}-${process.pid}-${randomUUID()}.tmp`);
  let handle = null;
  try {
    handle = await open(probePath, "wx");
    await handle.writeFile("ready\n", "utf8");
    await handle.sync();
    await handle.close();
    handle = null;
    await rm(probePath, { force: true });
    return true;
  } finally {
    try { await handle?.close(); } catch {}
    await rm(probePath, { force: true }).catch(() => {});
  }
};


const runExecutionStoreProbe = async () => {
  executionStoreHealth = await probeExecutionStoreHealth({
    jobsRoot,
    probeWritablePath,
    probeExecutionSlotStoreWritable,
    statfs,
    minimumFreeBytes: EXECUTION_STORE_MIN_FREE_BYTES,
    warningFreeBytes: EXECUTION_STORE_WARN_FREE_BYTES,
    warningFreePercent: EXECUTION_STORE_WARN_FREE_PERCENT,
  });
  return executionStoreHealth;
};
let executionStoreProbeTimer = null;
const scheduleExecutionStoreProbe = (delayMs) => {
  executionStoreProbeTimer = setTimeout(async () => {
    try { await runExecutionStoreProbe(); } catch {}
    scheduleExecutionStoreProbe(executionStoreHealth.ok ? EXECUTION_STORE_PROBE_HEALTHY_INTERVAL_MS : EXECUTION_STORE_PROBE_RETRY_INTERVAL_MS);
  }, delayMs);
  executionStoreProbeTimer.unref?.();
};
const executionStoreInitialProbe = runExecutionStoreProbe()
  .catch(() => executionStoreHealth)
  .finally(() => scheduleExecutionStoreProbe(executionStoreHealth.ok ? EXECUTION_STORE_PROBE_HEALTHY_INTERVAL_MS : EXECUTION_STORE_PROBE_RETRY_INTERVAL_MS));

const EXECUTION_SNAPSHOT_INTERVAL_MS = 1000;
const EXECUTION_SNAPSHOT_STALE_MS = 5000;
let executionSnapshotCache = { ok: false, sampledAtMs: 0, sampledAtUtc: null, snapshot: null, error: "scheduler snapshot has not completed yet" };
let executionSnapshotTimer = null;
const runExecutionSnapshot = async () => {
  const startedAt = Date.now();
  try {
    const snapshot = await getExecutionSlotSnapshot({
      maxConcurrent: config.mcpExecMaxConcurrent,
      reservedInteractive: config.mcpExecReservedInteractive,
      watchMaxConcurrent: config.mcpWatchMaxConcurrent,
      heavyCapacity: config.mcpExecHeavyCapacity,
    });
    executionSnapshotCache = { ok: true, sampledAtMs: Date.now(), sampledAtUtc: new Date().toISOString(), durationMs: Date.now() - startedAt, snapshot, error: null };
  } catch (error) {
    executionSnapshotCache = { ...executionSnapshotCache, ok: false, sampledAtMs: Date.now(), sampledAtUtc: new Date().toISOString(), durationMs: Date.now() - startedAt, error: error?.message ?? String(error) };
  }
  return executionSnapshotCache;
};
const scheduleExecutionSnapshot = (delayMs) => {
  executionSnapshotTimer = setTimeout(async () => {
    await runExecutionSnapshot();
    scheduleExecutionSnapshot(EXECUTION_SNAPSHOT_INTERVAL_MS);
  }, delayMs);
  executionSnapshotTimer.unref?.();
};
const executionSnapshotInitial = runExecutionSnapshot().finally(() => scheduleExecutionSnapshot(1000));
const cachedExecutionSnapshot = async () => {
  await executionSnapshotInitial.catch(() => {});
  const ageMs = Date.now() - Number(executionSnapshotCache.sampledAtMs || 0);
  if (!executionSnapshotCache.snapshot || ageMs >= EXECUTION_SNAPSHOT_STALE_MS) {
    throw new Error("cached scheduler snapshot is unavailable or stale");
  }
  return executionSnapshotCache.snapshot;
};

const eventLoopHistogram = monitorEventLoopDelay({ resolution: 20 });
eventLoopHistogram.enable();
let eventLoopWindow = null;
let timerDriftMaxMs = 0;
let expectedTimerTickMs = Date.now() + 1000;
const finiteMs = (nanoseconds) => Number.isFinite(nanoseconds) ? Math.round((nanoseconds / 1e6) * 100) / 100 : null;
const snapshotEventLoopWindow = () => {
  const snapshot = {
    sampledAtUtc: new Date().toISOString(),
    p50Ms: finiteMs(eventLoopHistogram.percentile(50)),
    p95Ms: finiteMs(eventLoopHistogram.percentile(95)),
    p99Ms: finiteMs(eventLoopHistogram.percentile(99)),
    maxMs: finiteMs(eventLoopHistogram.max),
    timerDriftMaxMs: Math.round(timerDriftMaxMs * 100) / 100,
  };
  eventLoopHistogram.reset();
  timerDriftMaxMs = 0;
  eventLoopWindow = snapshot;
  return snapshot;
};
const driftTimer = setInterval(() => {
  const now = Date.now();
  timerDriftMaxMs = Math.max(timerDriftMaxMs, Math.max(0, now - expectedTimerTickMs));
  expectedTimerTickMs = now + 1000;
}, 1000);
driftTimer.unref?.();
let eventLoopWindowTimer = null;
const persistEventLoopWindow = () => {
  const snapshot = snapshotEventLoopWindow();
  writeJsonStateFile(mcpPerformanceStatePath, {
    EventLoop: snapshot,
    Process: {
      Pid: process.pid,
      UptimeSeconds: Math.round(process.uptime() * 10) / 10,
      Memory: process.memoryUsage(),
    },
  }).catch(() => {});
  eventLoopWindowTimer = setTimeout(persistEventLoopWindow, 10000);
  eventLoopWindowTimer.unref?.();
};
eventLoopWindowTimer = setTimeout(persistEventLoopWindow, 5000);
eventLoopWindowTimer.unref?.();
const getMcpPerformanceSnapshot = () => ({
  eventLoop: eventLoopWindow ?? {
    sampledAtUtc: new Date().toISOString(),
    p50Ms: finiteMs(eventLoopHistogram.percentile(50)),
    p95Ms: finiteMs(eventLoopHistogram.percentile(95)),
    p99Ms: finiteMs(eventLoopHistogram.percentile(99)),
    maxMs: finiteMs(eventLoopHistogram.max),
    timerDriftMaxMs: Math.round(timerDriftMaxMs * 100) / 100,
  },
  process: {
    pid: process.pid,
    uptimeSeconds: Math.round(process.uptime() * 10) / 10,
    memory: process.memoryUsage(),
  },
});

const readStartupStatusSnapshot = async () => {
  try {
    return JSON.parse(await readFile(startupStatePath, "utf8"));
  } catch {
    return null;
  }
};

const readGuardianStatusSnapshot = async () => {
  try {
    const state = JSON.parse(await readFile(guardianStatePath, "utf8"));
    return {
      observedAtUtc: state.ObservedAtUtc ?? null,
      isHealthy: state.IsHealthy ?? null,
      needsRepair: state.NeedsRepair ?? null,
      mcpElevated: state.McpElevated ?? null,
      publicTunnelHealthy: state.PublicTunnelHealthy ?? null,
      cloudflaredRunning: state.CloudflaredRunning ?? null,
      cloudflaredMetrics: state.CloudflaredMetrics ?? null,
      cloudflaredMetricsDelta: state.CloudflaredMetricsDelta ?? null,
      tunnelTransportHealthy: state.TunnelTransportHealthy ?? null,
      tunnelTransportDegraded: state.TunnelTransportDegraded ?? false,
      tunnelTransportReasons: state.TunnelTransportReasons ?? [],
      readiness: state.Readiness ?? null,
      reasons: state.Reasons ?? [],
    };
  } catch {
    return null;
  }
};

const outputSchema = {
  ok: z.boolean(),
  summary: z.string(),
  data: z.any().optional(),
  stdout: z.string().optional(),
  stderr: z.string().optional(),
  exitCode: z.number().nullable().optional(),
  truncated: z.boolean().optional(),
};

const MAX_USAGE_PREVIEW_CHARS = 240;
export const MAX_TOOL_SUMMARY_CHARS = 4096;
export const SYNC_MCP_TIMEOUT_SECONDS = 90;
const syncCommandTimeoutSchema = () => z.number().int().min(1).max(SYNC_MCP_TIMEOUT_SECONDS).default(SYNC_MCP_TIMEOUT_SECONDS).describe(
  `Synchronous command timeout in seconds (maximum ${SYNC_MCP_TIMEOUT_SECONDS}). Use devbox_exec_start for longer work so upstream MCP request deadlines cannot cancel it.`,
);
const COMMAND_OUTPUT_LIMIT_CHARS = Math.max(100, config.maxTextOutputChars === null
  ? config.maxCommandOutputChars
  : Math.min(config.maxTextOutputChars, config.maxCommandOutputChars));
const INTERACTIVE_WAIT_MAX_SECONDS = Math.min(config.mcpWaitMaxSeconds, 85);
const WAIT_FOR_FILE_DEFAULT_SECONDS = Math.min(60, INTERACTIVE_WAIT_MAX_SECONDS);
const withInteractiveExecution = async ({ label, signal, command = "", program = "", args = [] }, callback) => {
  const inferredResourceClass = inferJobResourceClass({ command, program, args, requested: "auto" });
  const resourceClass = inferredResourceClass === "watch" ? "light" : inferredResourceClass;
  const weight = resourceClass === "heavy" ? config.mcpExecHeavyWeight : 1;
  return withExecutionSlot({
    kind: "interactive",
    label,
    maxConcurrent: config.mcpExecMaxConcurrent,
    reservedInteractive: config.mcpExecReservedInteractive,
    watchMaxConcurrent: config.mcpWatchMaxConcurrent,
    heavyCapacity: config.mcpExecHeavyCapacity,
    resourceClass,
    weight,
    queueTimeoutMs: config.mcpExecQueueTimeoutMs,
    signal,
  }, async (lease) => {
    try {
      return await callback(lease);
    } catch (error) {
      if (error && typeof error === "object") {
        error.data = {
          ...(error.data && typeof error.data === "object" ? error.data : {}),
          execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot },
        };
      }
      throw error;
    }
  });
};

const SENSITIVE_ARGUMENT_KEY = /(token|secret|password|authorization|cookie|content_base64|expected_sha256)/i;
const LARGE_TEXT_ARGUMENT_KEY = /^(command|content)$/i;
const INTERNAL_TOOL_ARGUMENT_KEYS = new Set([
  "signal",
  "sessionId",
  "_meta",
  "authInfo",
  "requestId",
  "requestInfo",
  "taskId",
  "taskStore",
  "taskRequestedTtl",
  "closeSSEStream",
  "closeStandaloneSSEStream",
]);

const limitedPositiveInteger = (description, defaultValue, maxValue) => {
  let schema = z.number().int().min(1);
  if (Number.isFinite(maxValue)) {
    schema = schema.max(Math.max(1, maxValue));
  }

  return schema.default(defaultValue).describe(description);
};

const transferByteCountSchema = (description, defaultValue) =>
  limitedPositiveInteger(description, defaultValue, config.maxMcpTransferChars);

const summarizeArgumentValue = (key, value) => {
  if (value === null || value === undefined) {
    return value ?? null;
  }

  if (typeof value === "string") {
    if (SENSITIVE_ARGUMENT_KEY.test(key)) {
      return {
        type: "string",
        length: value.length,
        redacted: true,
      };
    }

    const preview = value.length > MAX_USAGE_PREVIEW_CHARS ? `${value.slice(0, MAX_USAGE_PREVIEW_CHARS)}...` : value;
    return {
      type: "string",
      length: value.length,
      preview: LARGE_TEXT_ARGUMENT_KEY.test(key) ? preview : value.length > MAX_USAGE_PREVIEW_CHARS ? preview : value,
    };
  }

  if (Array.isArray(value)) {
    return {
      type: "array",
      length: value.length,
      sample: value.slice(0, 8).map((entry) => summarizeArgumentValue(`${key}[]`, entry)),
    };
  }

  if (typeof value === "object") {
    const entries = Object.entries(value).slice(0, 12);
    return Object.fromEntries(entries.map(([nestedKey, nestedValue]) => [nestedKey, summarizeArgumentValue(nestedKey, nestedValue)]));
  }

  return value;
};

const summarizeToolArguments = (args) => {
  if (!args || typeof args !== "object" || Array.isArray(args)) {
    return args ?? null;
  }

  const filteredEntries = Object.entries(args).filter(([key]) => !INTERNAL_TOOL_ARGUMENT_KEYS.has(key));
  return Object.fromEntries(filteredEntries.map(([key, value]) => [key, summarizeArgumentValue(key, value)]));
};

const summarizeToolContext = (args) => {
  if (!args || typeof args !== "object" || Array.isArray(args)) {
    return null;
  }

  const context = {};
  const authInfo = args.authInfo;
  const requestInfo = args.requestInfo;

  if (typeof authInfo?.clientId === "string" && authInfo.clientId.trim()) {
    context.client_id = authInfo.clientId;
  }

  if ((typeof args.requestId === "number" || typeof args.requestId === "string") && String(args.requestId).trim()) {
    context.request_id = args.requestId;
  }

  if (typeof args.sessionId === "string" && args.sessionId.trim()) {
    context.session_id = args.sessionId;
  }

  const userAgent = requestInfo?.headers?.["user-agent"];
  if (typeof userAgent === "string" && userAgent.trim()) {
    context.user_agent = userAgent;
  }

  return Object.keys(context).length > 0 ? context : null;
};

const boundedToolSummary = (value, fallback = "The command failed.") =>
  trimText(String(value || fallback), MAX_TOOL_SUMMARY_CHARS);

export const combineAbortSignals = (signals = []) => {
  const activeSignals = [...new Set(signals.filter(Boolean))];
  if (activeSignals.length === 0) {
    return { signal: undefined, dispose() {} };
  }
  if (activeSignals.length === 1) {
    return { signal: activeSignals[0], dispose() {} };
  }

  const controller = new AbortController();
  const listeners = [];
  const abortFrom = (source) => {
    if (!controller.signal.aborted) {
      controller.abort(source.reason);
    }
  };

  for (const source of activeSignals) {
    if (source.aborted) {
      abortFrom(source);
      break;
    }
    const listener = () => abortFrom(source);
    source.addEventListener("abort", listener, { once: true });
    listeners.push([source, listener]);
  }

  return {
    signal: controller.signal,
    dispose() {
      for (const [source, listener] of listeners) {
        source.removeEventListener("abort", listener);
      }
    },
  };
};

const getUsageLogState = async (logPath) => {
  let state = usageLogStates.get(logPath);
  if (!state) {
    state = { chain: Promise.resolve(), bytes: null, directoryReady: false };
    usageLogStates.set(logPath, state);
  }
  if (!state.directoryReady) {
    await mkdir(path.dirname(logPath), { recursive: true });
    state.directoryReady = true;
  }
  if (state.bytes === null) {
    try {
      state.bytes = (await stat(logPath)).size;
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
      state.bytes = 0;
    }
  }
  return state;
};

const rotateUsageLog = async (logPath) => {
  const rotations = Math.max(0, config.mcpUsageLogRotations);
  if (rotations <= 0) return;
  await rm(`${logPath}.${rotations}`, { force: true });
  for (let index = rotations - 1; index >= 1; index -= 1) {
    await rename(`${logPath}.${index}`, `${logPath}.${index + 1}`).catch((error) => {
      if (error?.code !== "ENOENT") throw error;
    });
  }
  await rename(logPath, `${logPath}.1`).catch((error) => {
    if (error?.code !== "ENOENT") throw error;
  });
};

const appendJsonlEvent = async (logPath, event) => {
  const state = await getUsageLogState(logPath);
  const line = `${JSON.stringify(event)}\n`;
  const lineBytes = Buffer.byteLength(line);
  const maxBytes = config.mcpUsageLogMaxBytes;
  const rotations = Math.max(0, config.mcpUsageLogRotations);
  const previous = state.chain;
  const next = previous
    .catch(() => {})
    .then(async () => {
      if (Number.isFinite(maxBytes) && maxBytes > 0 && rotations > 0 && state.bytes + lineBytes >= maxBytes) {
        await rotateUsageLog(logPath);
        state.bytes = 0;
      }
      await appendFile(logPath, line, "utf8");
      state.bytes += lineBytes;
    });
  state.chain = next;
  await next;
};

const writeJsonStateFile = async (filePath, value) => {
  await mkdir(path.dirname(filePath), { recursive: true });
  const tempFilePath = `${filePath}.${process.pid}.${randomUUID()}.tmp`;
  await writeFile(tempFilePath, `${JSON.stringify(value, null, 2)}\n`, "utf8");

  try {
    await rename(tempFilePath, filePath);
  } catch (error) {
    if (error?.code !== "EEXIST" && error?.code !== "EPERM") {
      throw error;
    }

    await rm(filePath, { force: true });
    await rename(tempFilePath, filePath);
  }
};

const setGuardianDesiredState = async (shouldRun, source) => {
  await writeJsonStateFile(guardianDesiredStatePath, {
    ShouldRun: Boolean(shouldRun),
    UpdatedAtUtc: new Date().toISOString(),
    Source: source,
  });
};

const logToolEvent = async (event) => {
  try {
    await appendJsonlEvent(toolUsageLogPath, event);
  } catch {
  }
};

const instrumentToolHandler = (toolName, handler, requestSignal) => async (args = {}, extra = {}) => {
  const invocationId = randomUUID();
  const startedAt = new Date().toISOString();
  const started = Date.now();
  const argumentSummary = summarizeToolArguments(args);
  const context = summarizeToolContext(extra);
  const combinedSignal = combineAbortSignals([extra?.signal, requestSignal]);
  const handlerExtra = { ...extra, signal: combinedSignal.signal };

  await logToolEvent({
    type: "tool_start",
    invocation_id: invocationId,
    tool: toolName,
    started_at: startedAt,
    arguments: argumentSummary,
    context,
  });

  try {
    const result = await handler(args, handlerExtra);
    const structured = result?.structuredContent ?? {};
    const loggedSummary = boundedToolSummary(structured.summary, "Tool completed.");
    const resultTextChars = Array.isArray(result?.content)
      ? result.content.reduce((total, entry) => total + (entry?.type === "text" ? String(entry.text ?? "").length : 0), 0)
      : 0;

    await logToolEvent({
      type: "tool_finish",
      invocation_id: invocationId,
      tool: toolName,
      started_at: startedAt,
      finished_at: new Date().toISOString(),
      duration_ms: Date.now() - started,
      ok: structured.ok ?? !result?.isError,
      is_error: Boolean(result?.isError),
      summary: loggedSummary.text || null,
      summary_truncated: loggedSummary.truncated,
      result_text_chars: resultTextChars,
      stdout_chars: String(structured.stdout ?? "").length,
      stderr_chars: String(structured.stderr ?? "").length,
      exit_code: structured.exitCode ?? null,
      truncated: Boolean(structured.truncated),
      queue_wait_ms: structured.data?.execution?.queue_wait_ms ?? null,
      execution_slot: structured.data?.execution?.slot ?? null,
      arguments: argumentSummary,
      context,
    });

    return result;
  } catch (error) {
    const loggedError = boundedToolSummary(error instanceof Error ? error.message : String(error));
    await logToolEvent({
      type: "tool_throw",
      invocation_id: invocationId,
      tool: toolName,
      started_at: startedAt,
      finished_at: new Date().toISOString(),
      duration_ms: Date.now() - started,
      error: loggedError.text,
      error_truncated: loggedError.truncated,
      arguments: argumentSummary,
      context,
    });
    throw error;
  } finally {
    combinedSignal.dispose();
  }
};

const withToolHints = (
  descriptor,
  {
    readOnlyHint,
    destructiveHint = false,
    openWorldHint = false,
    idempotentHint,
    invoking,
    invoked,
  },
) => ({
  ...descriptor,
  annotations: {
    ...descriptor.annotations,
    readOnlyHint,
    destructiveHint,
    openWorldHint,
    ...(idempotentHint === undefined ? {} : { idempotentHint }),
  },
  _meta: {
    ...descriptor._meta,
    "openai/toolInvocation/invoking": invoking,
    "openai/toolInvocation/invoked": invoked,
  },
});

const safeReadOnlyTool = (descriptor, invoking, invoked) =>
  withToolHints(descriptor, {
    readOnlyHint: true,
    destructiveHint: false,
    openWorldHint: false,
    idempotentHint: true,
    invoking,
    invoked,
  });

const safeActionTool = (descriptor, invoking, invoked) =>
  withToolHints(descriptor, {
    readOnlyHint: false,
    destructiveHint: false,
    openWorldHint: false,
    invoking,
    invoked,
  });

const textFromResult = (summary, data, stdout, stderr) => {
  const parts = [summary];

  if (data !== undefined) {
    parts.push(JSON.stringify(data, null, 2));
  }

  if (stdout) {
    parts.push(`stdout:\n${stdout}`);
  }

  if (stderr) {
    parts.push(`stderr:\n${stderr}`);
  }

  return parts.join("\n\n");
};

const successResult = (summary, extra = {}) => {
  const boundedSummary = boundedToolSummary(summary, "Tool completed.");
  const structuredContent = {
    ok: true,
    summary: boundedSummary.text,
    data: extra.data,
    stdout: extra.stdout,
    stderr: extra.stderr,
    exitCode: extra.exitCode ?? null,
    truncated: Boolean(extra.truncated) || boundedSummary.truncated,
  };

  return {
    content: [
      {
        type: "text",
        text: extra.text ?? textFromResult(boundedSummary.text, extra.data, extra.stdout, extra.stderr),
      },
    ],
    structuredContent,
  };
};

const imageCaptureResult = (summary, { image, jpeg, mimeType = "image/jpeg", metadata }) => {
  const bytes = image ?? jpeg;
  const result = successResult(summary, {
    data: metadata,
    text: textFromResult(summary, metadata),
  });
  result.content.push({
    type: "image",
    data: bytes.toString("base64"),
    mimeType,
  });
  return result;
};

const isCommandStyleError = (error) =>
  Boolean(
    error &&
      typeof error === "object" &&
      (error instanceof HostCommandError || "exitCode" in error || "stdout" in error || "stderr" in error),
  );

const errorResult = (error, fallbackSummary = "The command failed.") => {
  if (isCommandStyleError(error)) {
    const stdout = trimText(error.stdout, COMMAND_OUTPUT_LIMIT_CHARS);
    const stderr = trimText(error.stderr, COMMAND_OUTPUT_LIMIT_CHARS);
    const summary = boundedToolSummary(error.message, fallbackSummary);
    const data = error.data;

    return {
      content: [
        {
          type: "text",
          text: textFromResult(summary.text, data, stdout.text, stderr.text),
        },
      ],
      structuredContent: {
        ok: false,
        summary: summary.text,
        data,
        stdout: stdout.text,
        stderr: stderr.text,
        exitCode: error.exitCode ?? null,
        truncated: summary.truncated || stdout.truncated || stderr.truncated,
      },
      isError: true,
    };
  }

  const data = error?.data;
  const summary = boundedToolSummary(error instanceof Error ? error.message : fallbackSummary, fallbackSummary);
  return {
    content: [
      {
        type: "text",
        text: textFromResult(summary.text, data),
      },
    ],
    structuredContent: {
      ok: false,
      summary: summary.text,
      data,
      exitCode: null,
      truncated: summary.truncated,
    },
    isError: true,
  };
};

const fromProcessResult = (summary, result, extra = {}) => {
  const requestedMaxChars = Math.max(100, Number(extra.output?.maxChars) || COMMAND_OUTPUT_LIMIT_CHARS);
  const maxChars = Math.min(COMMAND_OUTPUT_LIMIT_CHARS, requestedMaxChars);
  const outputOptions = {
    mode: extra.output?.mode || "tail",
    maxChars,
    maxLines: Math.max(0, Number(extra.output?.maxLines) || 0),
  };
  const stdout = shapeProcessOutput(result.stdout, outputOptions);
  const stderr = shapeProcessOutput(result.stderr, outputOptions);
  const outputMetadata = {
    mode: outputOptions.mode,
    max_chars: maxChars,
    max_lines: outputOptions.maxLines,
    stdout_original_chars: Number.isFinite(result.stdoutOriginalChars) ? result.stdoutOriginalChars : stdout.originalChars,
    stderr_original_chars: Number.isFinite(result.stderrOriginalChars) ? result.stderrOriginalChars : stderr.originalChars,
  };
  if (typeof result.stdoutCaptureTruncated === "boolean") outputMetadata.stdout_capture_truncated = result.stdoutCaptureTruncated;
  if (typeof result.stderrCaptureTruncated === "boolean") outputMetadata.stderr_capture_truncated = result.stderrCaptureTruncated;
  const baseData = extra.data && typeof extra.data === "object" ? extra.data : {};

  return successResult(summary, {
    data: { ...baseData, output: outputMetadata },
    stdout: stdout.text || undefined,
    stderr: stderr.text || undefined,
    exitCode: result.exitCode ?? null,
    truncated: result.stdoutCaptureTruncated === true || result.stderrCaptureTruncated === true || stdout.truncated || stderr.truncated,
  });
};

const buildServer = ({ requestSignal } = {}) => {
  const server = new McpServer(
    {
      name: runtimeServerName,
      version,
      websiteUrl: "https://github.com/adybag14-cyber/devbox",
    },
    {
      capabilities: {
        logging: {},
      },
    },
  );

  const rawRegisterTool = server.registerTool.bind(server);
  server.registerTool = (name, descriptor, handler) =>
    rawRegisterTool(name, descriptor, instrumentToolHandler(name, handler, requestSignal));

  server.registerTool(
    "devbox_github_auth_status",
    safeReadOnlyTool(
      {
        title: `${runtimeTitle} GitHub Auth Status`,
        description: `Use this when you need to confirm whether the ${runtimeLabel} is authenticated to GitHub and which git identity is configured.`,
        outputSchema,
      },
      `Checking ${runtimeLabel} GitHub auth`,
      `${runtimeLabel} GitHub auth checked`,
    ),
    async () => {
      try {
        const data = await getDevboxGithubAuthStatus();
        return successResult(`Fetched ${runtimeLabel} GitHub auth status.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to fetch ${runtimeLabel} GitHub auth status.`);
      }
    },
  );

  server.registerTool(
    "devbox_sync_github_auth_from_host",
    safeActionTool(
      {
        title: `Sync Host GitHub Auth Into ${runtimeTitle}`,
        description:
          `Use this when the host already has a valid GitHub CLI login and the ${runtimeLabel} should inherit GitHub authentication and git identity from the host.`,
        outputSchema,
      },
      "Syncing host GitHub auth",
      "Host GitHub auth synced",
    ),
    async () => {
      try {
        const hostGithub = await getHostGithubAuthContext();
        const data = await syncGithubAuthToDevbox({
          token: hostGithub.token,
          userName: hostGithub.userName,
          userEmail: hostGithub.userEmail,
        });

        return successResult(`Synced the host GitHub CLI authentication into the ${runtimeLabel}.`, {
          data: {
            ...data,
            hostUserName: hostGithub.userName || null,
            hostUserEmail: hostGithub.userEmail || null,
          },
        });
      } catch (error) {
        return errorResult(error, `Failed to sync host GitHub authentication into the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_status",
    safeReadOnlyTool(
      {
        title: `${runtimeTitle} Status`,
        description: `Use this when you need the current state of the ${runtimeLabel} and its installed toolchain.`,
        outputSchema,
      },
      `Checking ${runtimeLabel} status`,
      `${runtimeLabel} status ready`,
    ),
    async () => {
      try {
        const info = await getDevboxInfo();
        const data = {
          ...info,
          hostWorkspacePath: config.hostWorkspacePath,
          devboxWorkspacePath: config.devboxWorkspacePath,
          hostExecEnabled: config.enableHostExec,
          guardian: await readGuardianStatusSnapshot(),
          startup: await readStartupStatusSnapshot(),
          execution: await cachedExecutionSnapshot(),
          performance: getMcpPerformanceSnapshot(),
          executionStore: {
            ...executionStoreHealth,
            ageMs: Math.max(0, Date.now() - Number(executionStoreHealth.sampledAtMs || 0)),
            stale: Date.now() - Number(executionStoreHealth.sampledAtMs || 0) >= EXECUTION_STORE_PROBE_STALE_MS,
          },
          operationalWarnings: executionStoreHealth.diskPressure === "warning" ? ["disk-pressure"] : [],
        };

        if (info.running) {
          const versions = getCachedDevboxVersions();
          data.versions = versions;
          data.versionsCached = versions !== null;
        }

        return successResult(`Fetched ${runtimeLabel} status.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to fetch ${runtimeLabel} status.`);
      }
    },
  );

  server.registerTool(
    "devbox_start",
    safeActionTool(
      {
        title: `Start ${runtimeTitle}`,
        description: `Use this when the ${runtimeLabel} is stopped or missing and needs to be brought online.`,
        outputSchema,
      },
      `Starting ${runtimeLabel}`,
      `${runtimeLabel} started`,
    ),
    async () => {
      try {
        await setGuardianDesiredState(true, "src/server.js:devbox_start");
        const info = await ensureDevboxRunning();
        const summary = isDockerRuntime
          ? `${runtimeTitle} ${info.name} is running.`
          : `${runtimeTitle} is ready in the current server process.`;
        return successResult(summary, { data: info });
      } catch (error) {
        return errorResult(error, `Failed to start the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_stop",
    safeActionTool(
      {
        title: `Stop ${runtimeTitle}`,
        description: `Use this when the ${runtimeLabel} should be shut down without deleting the workspace.`,
        outputSchema,
      },
      `Stopping ${runtimeLabel}`,
      `${runtimeLabel} stopped`,
    ),
    async () => {
      try {
        await setGuardianDesiredState(false, "src/server.js:devbox_stop");
        const info = await stopDevbox();
        const summary = isDockerRuntime
          ? `${runtimeTitle} ${info.name} is stopped.`
          : info.controlMessage || `${runtimeTitle} stop is managed by the launcher command.`;
        return successResult(summary, { data: info });
      } catch (error) {
        return errorResult(error, `Failed to stop the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_restart",
    safeActionTool(
      {
        title: `Restart ${runtimeTitle}`,
        description: `Use this when the ${runtimeLabel} needs a clean restart.`,
        outputSchema,
      },
      `Restarting ${runtimeLabel}`,
      `${runtimeLabel} restarted`,
    ),
    async () => {
      try {
        await setGuardianDesiredState(true, "src/server.js:devbox_restart");
        const info = await restartDevbox();
        const summary = isDockerRuntime
          ? `${runtimeTitle} ${info.name} has been restarted.`
          : info.controlMessage || `${runtimeTitle} restart is managed by the launcher command.`;
        return successResult(summary, { data: info });
      } catch (error) {
        return errorResult(error, `Failed to restart the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_recreate",
    safeActionTool(
      {
        title: `Recreate ${runtimeTitle}`,
        description: `Use this when the ${runtimeLabel} should be rebuilt from the configured backend while preserving the workspace.`,
        outputSchema,
      },
      `Recreating ${runtimeLabel}`,
      `${runtimeLabel} recreated`,
    ),
    async () => {
      try {
        await setGuardianDesiredState(true, "src/server.js:devbox_recreate");
        const info = await recreateDevbox();
        const summary = isDockerRuntime
          ? `${runtimeTitle} ${info.name} has been recreated.`
          : info.controlMessage || `${runtimeTitle} recreation is managed by the launcher command.`;
        return successResult(summary, { data: info });
      } catch (error) {
        return errorResult(error, `Failed to recreate the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_exec_readonly",
    safeReadOnlyTool(
      {
        title: `Run Read-Only Shell Command In ${runtimeTitle}`,
        description: isDockerRuntime
          ? `Use this only when inspection requires shell syntax such as pipelines, redirection, variables, or compound commands. Prefer devbox_run_program for a single executable such as git, gh, node, python, or rg because it avoids shell startup overhead. It runs in the long-lived devbox container; read-only behavior is advisory. Synchronous calls are capped at ${SYNC_MCP_TIMEOUT_SECONDS}s; use devbox_exec_start for longer work.`
          : `Use this only when inspection requires shell syntax such as pipelines, redirection, variables, or compound commands. Prefer devbox_run_program for a single executable such as git, gh, node, python, or rg because it avoids shell startup overhead. In ${runtimeLabel} mode this runs directly on the host shell, so read-only behavior is advisory rather than sandbox-enforced. Synchronous calls are capped at ${SYNC_MCP_TIMEOUT_SECONDS}s; use devbox_exec_start for longer work.`,
        inputSchema: {
          command: z.string().min(1).describe(`Read-only shell command to run inside the ${runtimeLabel}.`),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return. Use tail for logs, head for headers, or summary for both ends."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the disposable read-only container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running read-only shell command in ${runtimeLabel}`,
      `Read-only ${runtimeLabel} shell command finished`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, output_mode: outputMode, max_output_chars: maxOutputChars, max_output_lines: maxOutputLines, user }, extra) => {
      try {
        return await withInteractiveExecution({ label: "devbox_exec_readonly", signal: extra?.signal, command }, async (lease) => {
          const result = await execReadOnlyInDevbox({
            command,
            workingDir,
            timeoutMs: (timeoutSeconds + 5) * 1000,
            user,
            signal: extra?.signal,
            maxCaptureChars: Math.max(COMMAND_OUTPUT_LIMIT_CHARS * 2, Number(maxOutputChars || 0) * 2),
          });
          return fromProcessResult(`Ran a read-only shell command in the ${runtimeLabel} at ${workingDir}.`, result, {
            data: { execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot } },
            output: { mode: outputMode, maxChars: maxOutputChars, maxLines: maxOutputLines },
          });
        });
      } catch (error) {
        return errorResult(error, `Failed to run the read-only ${runtimeLabel} shell command.`);
      }
    },
  );

  server.registerTool(
    "devbox_exec",
    safeActionTool(
      {
        title: `Run Mutating Shell Command In ${runtimeTitle}`,
        description: isDockerRuntime
          ? `Use this only when the shell command needs side effects such as writing files, building artifacts, installing packages, changing git state, or otherwise mutating the devbox or workspace. Prefer devbox_run_program for a single executable and devbox_exec_readonly for shell-based inspection. Synchronous calls are capped at ${SYNC_MCP_TIMEOUT_SECONDS}s; use devbox_exec_start for builds or other longer work.`
          : `Use this when the shell command needs side effects such as writing files, building artifacts, installing packages, changing git state, or otherwise mutating the ${runtimeLabel}. Prefer devbox_run_program for a single executable and devbox_exec_readonly for shell-based inspection. Synchronous calls are capped at ${SYNC_MCP_TIMEOUT_SECONDS}s; use devbox_exec_start for builds or other longer work.`,
        inputSchema: {
          command: z.string().min(1).describe(`Shell command to run inside the ${runtimeLabel}.`),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return. Use tail for logs, head for headers, or summary for both ends."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the devbox container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running shell command in ${runtimeLabel}`,
      `${runtimeLabel} shell command finished`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, output_mode: outputMode, max_output_chars: maxOutputChars, max_output_lines: maxOutputLines, user }, extra) => {
      try {
        return await withInteractiveExecution({ label: "devbox_exec", signal: extra?.signal, command }, async (lease) => {
          const result = await execInDevbox({
            command,
            workingDir,
            timeoutMs: (timeoutSeconds + 5) * 1000,
            user,
            signal: extra?.signal,
            maxCaptureChars: Math.max(COMMAND_OUTPUT_LIMIT_CHARS * 2, Number(maxOutputChars || 0) * 2),
          });
          return fromProcessResult(`Ran a shell command in the ${runtimeLabel} at ${workingDir}.`, result, {
            data: { execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot } },
            output: { mode: outputMode, maxChars: maxOutputChars, maxLines: maxOutputLines },
          });
        });
      } catch (error) {
        return errorResult(error, `Failed to run the ${runtimeLabel} shell command.`);
      }
    },
  );

  server.registerTool(
    "devbox_run_program",
    safeActionTool(
      {
        title: `Run Program Directly In ${runtimeTitle}`,
        description: `Preferred fast path for running one executable with structured arguments inside the ${runtimeLabel} (for example git, gh, node, python, or rg). This avoids shell startup and quoting overhead. Use devbox_exec or devbox_exec_readonly only when shell syntax is actually required.`,
        inputSchema: {
          program: z.string().min(1).describe("Allowed executable name."),
          args: z.array(z.string()).default([]).describe("Argument list passed directly to the executable without shell parsing."),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return. Use tail for logs, head for headers, or summary for both ends."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the devbox container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running direct program in ${runtimeLabel}`,
      `${runtimeLabel} direct program finished`,
    ),
    async ({ program, args, working_dir: workingDir, timeout_seconds: timeoutSeconds, output_mode: outputMode, max_output_chars: maxOutputChars, max_output_lines: maxOutputLines, user }, extra) => {
      try {
        return await withInteractiveExecution({ label: "devbox_run_program", signal: extra?.signal, program, args }, async (lease) => {
          const result = await runProgramInDevbox({
            program,
            args,
            workingDir,
            timeoutMs: (timeoutSeconds + 5) * 1000,
            user,
            signal: extra?.signal,
            maxCaptureChars: Math.max(COMMAND_OUTPUT_LIMIT_CHARS * 2, Number(maxOutputChars || 0) * 2),
          });
          return fromProcessResult(`Ran ${program} directly in the ${runtimeLabel}.`, result, {
            data: { execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot } },
            output: { mode: outputMode, maxChars: maxOutputChars, maxLines: maxOutputLines },
          });
        });
      } catch (error) {
        return errorResult(error, `Failed to run ${program} directly in the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_exec_start",
    safeActionTool(
      {
        title: `Start Background Shell Job In ${runtimeTitle}`,
        description: `Use this for ${runtimeLabel} commands expected to run longer than about 90 seconds. It starts a detached persisted job and returns immediately so connector request deadlines do not cancel the work.`,
        inputSchema: {
          command: z.string().min(1).describe(`Shell command to run asynchronously inside the ${runtimeLabel}.`),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: z.number().int().min(1).max(86400).default(7200).describe("Maximum background job runtime in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the devbox container." : `Host user hint for ${runtimeLabel} mode.`),
          read_only: z.boolean().default(false).describe("When true, use the read-only execution path. Read-only enforcement is advisory in host mode."),
          resource_class: z.enum(["auto", "watch", "light", "heavy"]).default("auto").describe("Scheduling class. auto detects passive watches and heavy build/browser workloads."),
        },
        outputSchema,
      },
      `Starting background job in ${runtimeLabel}`,
      `${runtimeLabel} background job started`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user, read_only: readOnly, resource_class: resourceClass }) => {
      try {
        const data = await startDevboxJob({ command, workingDir, timeoutSeconds, user, readOnly, resourceClass });
        return successResult(`Started background ${runtimeLabel} job ${data.id}.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to start the background ${runtimeLabel} job.`);
      }
    },
  );

  server.registerTool(
    "devbox_run_program_start",
    safeActionTool(
      {
        title: `Start Background Program Directly In ${runtimeTitle}`,
        description: `Preferred detached path for one long-running executable. It avoids shell startup/quoting and participates in weighted scheduling. Use resource_class=watch for passive watchers and heavy for builds/browser workloads.`,
        inputSchema: {
          program: z.string().min(1).describe("Allowed executable name."),
          args: z.array(z.string()).default([]).describe("Argument list passed directly without shell parsing."),
          input: z.string().optional().describe("Optional stdin text for the program."),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: z.number().int().min(1).max(86400).default(7200).describe("Maximum background runtime in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the devbox container." : `Host user hint for ${runtimeLabel} mode.`),
          resource_class: z.enum(["auto", "watch", "light", "heavy"]).default("auto").describe("Scheduling class. auto detects gh run watch and common heavy build/browser programs."),
        },
        outputSchema,
      },
      `Starting direct background program in ${runtimeLabel}`,
      `${runtimeLabel} direct background program started`,
    ),
    async ({ program, args, input, working_dir: workingDir, timeout_seconds: timeoutSeconds, user, resource_class: resourceClass }) => {
      try {
        const data = await startDevboxProgramJob({ program, args, input, workingDir, timeoutSeconds, user, resourceClass });
        return successResult(`Started direct background ${runtimeLabel} job ${data.id}.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to start direct background program in ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_job_status",
    safeReadOnlyTool(
      {
        title: "Get Devbox Background Job Status",
        description: "Read background-job status. Prefer wait_seconds instead of Start-Sleep polling; waiting here uses only a Node timer and consumes no execution slot or shell process.",
        inputSchema: {
          job_id: z.string().min(8).describe("Job id returned by devbox_exec_start or devbox_run_program_start."),
          wait_seconds: z.number().int().min(0).max(INTERACTIVE_WAIT_MAX_SECONDS).default(0).describe("Long-poll for a status change/terminal state without consuming an execution slot."),
          terminal_only: z.boolean().default(true).describe("When waiting, return early only for a terminal state; false also returns on queued to running transitions."),
        },
        outputSchema,
      },
      "Checking Devbox background job",
      "Devbox background job checked",
    ),
    async ({ job_id: jobId, wait_seconds: waitSeconds, terminal_only: terminalOnly }, extra) => {
      try {
        const data = waitSeconds > 0
          ? await waitForDevboxJobStatus(jobId, { waitSeconds, terminalOnly, signal: extra?.signal })
          : await getDevboxJobStatus(jobId);
        return successResult(`Background job ${jobId} is ${data.status}.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to read background job ${jobId}.`);
      }
    },
  );

  server.registerTool(
    "devbox_job_logs",
    safeReadOnlyTool(
      {
        title: "Read Devbox Background Job Logs",
        description: "Use this to retrieve bounded stdout/stderr tails for a job started by devbox_exec_start.",
        inputSchema: {
          job_id: z.string().min(8).describe("Job id returned by devbox_exec_start."),
          max_chars: z.number().int().min(100).max(100000).default(20000).describe("Maximum characters returned from each stdout/stderr tail."),
        },
        outputSchema,
      },
      "Reading Devbox background job logs",
      "Devbox background job logs read",
    ),
    async ({ job_id: jobId, max_chars: maxChars }) => {
      try {
        const data = await getDevboxJobLogs({ jobId, maxChars });
        return successResult(`Read logs for background job ${jobId}.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to read logs for background job ${jobId}.`);
      }
    },
  );

  server.registerTool(
    "devbox_job_cancel",
    safeActionTool(
      {
        title: "Cancel Devbox Background Job",
        description: "Use this to stop a job started by devbox_exec_start. The detached runner and its process tree are terminated.",
        inputSchema: {
          job_id: z.string().min(8).describe("Job id returned by devbox_exec_start."),
        },
        outputSchema,
      },
      "Cancelling Devbox background job",
      "Devbox background job cancelled",
    ),
    async ({ job_id: jobId }) => {
      try {
        const data = await cancelDevboxJob(jobId);
        return successResult(`Background job ${jobId} is ${data.status}.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to cancel background job ${jobId}.`);
      }
    },
  );

  server.registerTool(
    "devbox_wait",
    safeReadOnlyTool(
      {
        title: "Wait Without Consuming An Execution Slot",
        description: "Use this instead of Start-Sleep/sleep when you simply need to wait before the next MCP action. It uses a Node timer only: no PowerShell process and no execution slot.",
        inputSchema: {
          seconds: z.number().min(0.05).max(INTERACTIVE_WAIT_MAX_SECONDS).describe("How long to wait."),
          reason: z.string().max(200).default("").describe("Optional human-readable reason for telemetry/context."),
        },
        outputSchema,
      },
      "Waiting without occupying an execution slot",
      "Wait completed",
    ),
    async ({ seconds, reason }, extra) => {
      const started = Date.now();
      try {
        await abortableSleep(Math.round(seconds * 1000), extra?.signal);
        return successResult(`Waited ${seconds} seconds without an execution process.`, { data: { waited_ms: Date.now() - started, reason: reason || null } });
      } catch (error) {
        return errorResult(error, "Wait was cancelled.");
      }
    },
  );

  server.registerTool(
    "devbox_wait_for_file",
    safeReadOnlyTool(
      {
        title: "Wait For Host File Condition Without A Shell",
        description: "Host-mode filesystem condition wait using Node fs polling only. Prefer this over Start-Sleep followed by Test-Path/Get-Content. Docker mode should use devbox_wait because container paths are not directly visible to the MCP process.",
        inputSchema: {
          path: z.string().min(1).describe("Host filesystem path to watch."),
          should_exist: z.boolean().default(true).describe("Wait for the path to exist; false waits for removal."),
          min_bytes: z.number().int().min(0).default(0).describe("When waiting for existence, require at least this file size."),
          stable_ms: z.number().int().min(0).max(30000).default(0).describe("Require the condition to remain true for this duration."),
          timeout_seconds: z.number().min(0.1).max(INTERACTIVE_WAIT_MAX_SECONDS).default(WAIT_FOR_FILE_DEFAULT_SECONDS).describe("Maximum wait duration."),
          poll_ms: z.number().int().min(50).max(5000).default(250).describe("Filesystem poll interval."),
        },
        outputSchema,
      },
      "Waiting for host file condition",
      "Host file wait completed",
    ),
    async ({ path: filePath, should_exist: shouldExist, min_bytes: minBytes, stable_ms: stableMs, timeout_seconds: timeoutSeconds, poll_ms: pollMs }, extra) => {
      try {
        if (isDockerRuntime) throw new Error("devbox_wait_for_file is a no-subprocess host-mode primitive; use devbox_wait in Docker mode.");
        const data = await waitForPathCondition({ path: filePath, shouldExist, minBytes, stableMs, timeoutMs: timeoutSeconds * 1000, pollMs, signal: extra?.signal });
        return successResult(data.conditionMet ? `File condition satisfied for ${filePath}.` : `Timed out waiting for file condition at ${filePath}.`, { data });
      } catch (error) {
        return errorResult(error, `Failed while waiting for ${filePath}.`);
      }
    },
  );

  server.registerTool(
    "devbox_list_files",
    safeReadOnlyTool(
      {
        title: `List ${runtimeTitle} Files`,
        description: `Use this when you need a directory listing inside the ${runtimeLabel} workspace or another configured path.`,
        inputSchema: {
          path: z.string().default(config.devboxWorkspacePath).describe(`Directory path inside the ${runtimeLabel}.`),
          recursive: z.boolean().default(false).describe("When true, recurse into subdirectories."),
          max_depth: z.number().int().min(1).max(20).default(4).describe("Maximum recursive depth when recursive is true."),
          max_entries: z.number().int().min(1).max(50000).default(5000).describe("Maximum entries to return before stopping traversal."),
          timeout_seconds: z.number().int().min(1).max(300).default(30).describe("Maximum time spent traversing directories."),
          exclude_directories: z.array(z.string().min(1)).max(32)
            .default([".git", "node_modules", ".cache", ".venv", "venv", "__pycache__"])
            .describe("Directory names to prune from recursive traversal."),
        },
        outputSchema,
      },
      `Listing ${runtimeLabel} files`,
      `${runtimeLabel} files listed`,
    ),
    async ({
      path,
      recursive,
      max_depth: maxDepth,
      max_entries: maxEntries,
      timeout_seconds: timeoutSeconds,
      exclude_directories: excludeDirectories,
    }, extra) => {
      try {
        const result = await listFilesInDevbox({
          path,
          recursive,
          maxDepth,
          maxEntries,
          timeoutMs: timeoutSeconds * 1000,
          excludeDirectories,
          signal: extra?.signal,
        });
        return fromProcessResult(`Listed files in ${path}.`, result);
      } catch (error) {
        return errorResult(error, `Failed to list files in ${path}.`);
      }
    },
  );

  server.registerTool(
    "devbox_read_file",
    safeReadOnlyTool(
      {
        title: `Read ${runtimeTitle} File`,
        description: `Use this when you need text content from a file inside the ${runtimeLabel}.`,
        inputSchema: {
          path: z.string().min(1).describe(`File path inside the ${runtimeLabel}.`),
          max_bytes: transferByteCountSchema("Maximum bytes to return.", 65536),
        },
        outputSchema,
      },
      `Reading ${runtimeLabel} file`,
      `${runtimeLabel} file read`,
    ),
    async ({ path, max_bytes: maxBytes }) => {
      try {
        const result = await readFileInDevbox({ path, maxBytes });
        return fromProcessResult(`Read ${path} from the ${runtimeLabel}.`, result);
      } catch (error) {
        return errorResult(error, `Failed to read ${path} from the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_read_large_file",
    safeReadOnlyTool(
      {
        title: `Read Large ${runtimeTitle} File Chunk`,
        description: `Use this when you need an exact byte range from a larger file inside the ${runtimeLabel}. It returns a base64 chunk plus metadata for safe paging without UTF-8 corruption.`,
        inputSchema: {
          path: z.string().min(1).describe(`File path inside the ${runtimeLabel}.`),
          offset_bytes: z.number().int().min(0).default(0).describe("Starting byte offset within the file."),
          max_bytes: transferByteCountSchema("Maximum raw bytes to return from that offset.", 262144),
        },
        outputSchema,
      },
      `Reading large ${runtimeLabel} file chunk`,
      `Large ${runtimeLabel} file chunk read`,
    ),
    async ({ path, offset_bytes: offsetBytes, max_bytes: maxBytes }) => {
      try {
        const data = await readLargeFileInDevbox({ path, offsetBytes, maxBytes });
        const summary = `Read ${path} from byte ${offsetBytes} in the ${runtimeLabel}.`;
        return successResult(summary, {
          data,
          text: textFromResult(summary, summarizeLargeReadData(data)),
        });
      } catch (error) {
        return errorResult(error, `Failed to read ${path} from byte ${offsetBytes} in the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_write_file",
    safeActionTool(
      {
        title: `Write ${runtimeTitle} File`,
        description: `Use this when you need to create or overwrite a text file inside the ${runtimeLabel} workspace.`,
        inputSchema: {
          path: z.string().min(1).describe(`File path inside the ${runtimeLabel}.`),
          content: z.string().describe("UTF-8 file contents to write."),
          append: z.boolean().default(false).describe("Append to the file instead of overwriting it."),
          create_dirs: z.boolean().default(true).describe("Create parent directories if they do not exist."),
        },
        outputSchema,
      },
      `Writing ${runtimeLabel} file`,
      `${runtimeLabel} file written`,
    ),
    async ({ path, content, append, create_dirs: createDirs }) => {
      try {
        const result = await writeFileInDevbox({ path, content, append, createDirs });
        const summary = append ? `Appended text to ${path} in the ${runtimeLabel}.` : `Wrote ${path} in the ${runtimeLabel}.`;
        return fromProcessResult(summary, result);
      } catch (error) {
        return errorResult(error, `Failed to write ${path} in the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_write_large_file",
    safeActionTool(
      {
        title: `Write Large ${runtimeTitle} File`,
        description: `Use this when you need to create or overwrite a large file inside the ${runtimeLabel} workspace using base64-backed transfer with post-write verification.`,
        inputSchema: {
          path: z.string().min(1).describe(`File path inside the ${runtimeLabel}.`),
          content: z.string().optional().describe("Optional UTF-8 text payload to write. Prefer content_base64 for exact byte preservation."),
          content_base64: z.string().optional().describe("Base64-encoded raw bytes to write exactly as provided."),
          append: z.boolean().default(false).describe("Append to the file instead of overwriting it."),
          create_dirs: z.boolean().default(true).describe("Create parent directories if they do not exist."),
          expected_sha256: z.string().regex(/^[A-Fa-f0-9]{64}$/).optional().describe("Optional expected SHA-256 of the decoded payload for end-to-end verification."),
        },
        outputSchema,
      },
      `Writing large ${runtimeLabel} file`,
      `Large ${runtimeLabel} file written`,
    ),
    async ({ path, content, content_base64: contentBase64, append, create_dirs: createDirs, expected_sha256: expectedSha256 }) => {
      try {
        const normalizedPayload = normalizeLargeWritePayload({ content, contentBase64 });
        const data = await writeLargeFileInDevbox({
          path,
          contentBase64: normalizedPayload,
          append,
          createDirs,
          expectedSha256,
        });
        const summary = append
          ? `Appended large payload to ${path} in the ${runtimeLabel} and verified the exact bytes.`
          : `Wrote large payload to ${path} in the ${runtimeLabel} and verified the exact bytes.`;
        return successResult(summary, {
          data,
          text: textFromResult(summary, summarizeLargeWriteData(data)),
        });
      } catch (error) {
        return errorResult(error, `Failed to write large payload to ${path} in the ${runtimeLabel}.`);
      }
    },
  );

  server.registerTool(
    "devbox_search_files",
    safeReadOnlyTool(
      {
        title: `Search ${runtimeTitle} Files`,
        description: isDockerRuntime
          ? "Use this when you need ripgrep-style text search inside the Docker devbox workspace."
          : `Use this when you need text search inside the ${runtimeLabel} workspace. ripgrep-style syntax is supported best-effort in host mode.`,
        inputSchema: {
          pattern: z.string().min(1).describe("Search pattern for ripgrep or regex-style matching."),
          path: z.string().default(config.devboxWorkspacePath).describe("Directory path to search."),
          glob: z.string().default("*").describe("Optional glob filter."),
          case_sensitive: z.boolean().default(false).describe("When true, use case-sensitive search."),
          max_matches: z.number().int().min(1).max(5000).default(200).describe("Maximum number of matches to return."),
          max_depth: z.number().int().min(1).max(50).default(12).describe("Maximum recursive directory depth."),
          max_file_bytes: z.number().int().min(1).max(64 * 1024 * 1024).default(2 * 1024 * 1024)
            .describe("Skip files larger than this byte count."),
          timeout_seconds: z.number().int().min(1).max(300).default(30).describe("Maximum time spent searching."),
          exclude_directories: z.array(z.string().min(1)).max(32)
            .default([".git", "node_modules", ".cache", ".venv", "venv", "__pycache__"])
            .describe("Directory names to prune before searching."),
          include_ignored: z.boolean().default(false)
            .describe("When true, include hidden and ignore-file-excluded content. This is slower and should be used only for exhaustive searches."),
        },
        outputSchema,
      },
      `Searching ${runtimeLabel} files`,
      `${runtimeLabel} search finished`,
    ),
    async ({
      pattern,
      path,
      glob,
      case_sensitive: caseSensitive,
      max_matches: maxMatches,
      max_depth: maxDepth,
      max_file_bytes: maxBytesPerFile,
      timeout_seconds: timeoutSeconds,
      exclude_directories: excludeDirectories,
      include_ignored: includeIgnored,
    }, extra) => {
      try {
        return await withInteractiveExecution({ label: "devbox_search_files", signal: extra?.signal }, async (lease) => {
          const result = await searchFilesInDevbox({
            pattern,
            path,
            glob,
            caseSensitive,
            maxMatches,
            maxDepth,
            maxBytesPerFile,
            timeoutMs: timeoutSeconds * 1000,
            excludeDirectories,
            includeIgnored,
            signal: extra?.signal,
          });
          return fromProcessResult(`Searched ${path} for "${pattern}" inside the ${runtimeLabel}.`, result, {
            data: { execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot } },
          });
        });
      } catch (error) {
        return errorResult(error, `Failed to search ${path} inside the ${runtimeLabel}.`);
      }
    },
  );

  const hostStatusHandler = async () => {
    try {
      return successResult(`Fetched ${hostTitle.toLowerCase()} tool status.`, {
        data: getHostToolStatus(),
      });
    } catch (error) {
      return errorResult(error, `Failed to fetch ${hostTitle.toLowerCase()} tool status.`);
    }
  };

  const hostExecHandler = async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, output_mode: outputMode, max_output_chars: maxOutputChars, max_output_lines: maxOutputLines }, extra) => {
    try {
      return await withInteractiveExecution({ label: "host_exec", signal: extra?.signal, command }, async (lease) => {
        const result = await runHostShellCommand({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          signal: extra?.signal,
          maxCaptureChars: Math.max(COMMAND_OUTPUT_LIMIT_CHARS * 2, Number(maxOutputChars || 0) * 2),
        });
        return fromProcessResult(`Ran a ${hostCommandTitle.toLowerCase()} command in ${workingDir}.`, result, {
          data: { execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot } },
          output: { mode: outputMode, maxChars: maxOutputChars, maxLines: maxOutputLines },
        });
      });
    } catch (error) {
      return errorResult(error, `Failed to run the ${hostCommandTitle.toLowerCase()} command.`);
    }
  };

  const hostRunProgramHandler = async ({ program, args, working_dir: workingDir, timeout_seconds: timeoutSeconds, output_mode: outputMode, max_output_chars: maxOutputChars, max_output_lines: maxOutputLines }, extra) => {
    try {
      return await withInteractiveExecution({ label: "host_run_program", signal: extra?.signal, program, args }, async (lease) => {
        const result = await runAllowedProgram({
          program,
          args,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          signal: extra?.signal,
          maxCaptureChars: Math.max(COMMAND_OUTPUT_LIMIT_CHARS * 2, Number(maxOutputChars || 0) * 2),
        });
        return fromProcessResult(`Ran ${program} on the ${hostTitle.toLowerCase()}.`, result, {
          data: { execution: { queue_wait_ms: lease.queueWaitMs, slot: lease.slot } },
          output: { mode: outputMode, maxChars: maxOutputChars, maxLines: maxOutputLines },
        });
      });
    } catch (error) {
      return errorResult(error, `Failed to run ${program} on the ${hostTitle.toLowerCase()}.`);
    }
  };

  server.registerTool(
    "host_status",
    safeReadOnlyTool(
      {
        title: `${hostTitle} Tool Status`,
        description: `Use this when you need to inspect whether ${hostTitle.toLowerCase()} execution is enabled and which native programs are allowed.`,
        outputSchema,
      },
      `Checking ${hostTitle.toLowerCase()} tool status`,
      `${hostTitle} tool status ready`,
    ),
    hostStatusHandler,
  );

  server.registerTool(
    "windows_host_status",
    safeReadOnlyTool(
      {
        title: `${hostTitle} Tool Status`,
        description: `Compatibility alias for host_status. Use this when you need to inspect whether ${hostTitle.toLowerCase()} execution is enabled and which native programs are allowed.`,
        outputSchema,
      },
      `Checking ${hostTitle.toLowerCase()} tool status`,
      `${hostTitle} tool status ready`,
    ),
    hostStatusHandler,
  );

  const captureDisplayHandler = async ({ quality }, extra) => {
    try {
      const capture = await captureHostDisplay({ quality, signal: extra?.signal });
      return imageCaptureResult(`Captured the ${hostTitle.toLowerCase()} display.`, capture);
    } catch (error) {
      return errorResult(error, `Failed to capture the ${hostTitle.toLowerCase()} display.`);
    }
  };

  const captureWindowHandler = async ({ pid, quality, include_process_tree: includeProcessTree }, extra) => {
    try {
      const capture = await captureHostProgram({ pid, quality, includeProcessTree, signal: extra?.signal });
      return imageCaptureResult(`Captured ${hostTitle} window for PID ${pid}.`, capture);
    } catch (error) {
      return errorResult(error, `Failed to capture ${hostTitle} window for PID ${pid}.`);
    }
  };

  const displayCaptureTool = {
    title: `Capture ${hostTitle} Display`,
    description:
      `Capture the complete ${hostTitle.toLowerCase()} desktop using the native compositor/screenshot backend and return an MCP image content block. Windows returns JPEG; macOS/Linux use lossless PNG when their native tools do.`,
    inputSchema: {
      quality: z.number().int().min(1).max(100).default(85).describe("Requested image quality from 1 through 100. Native lossless PNG backends record but do not apply JPEG quality."),
    },
    outputSchema,
  };

  const windowCaptureTool = {
    title: `Capture ${hostTitle} Window by PID`,
    description:
      "Capture the largest visible window owned by a host PID or one of its child processes. The Windows backend detects black PrintWindow frames from GPU/DirectComposition surfaces and falls back to compositor-visible pixels. macOS uses CoreGraphics window discovery plus screencapture; Linux supports X11 and compositor-specific Wayland paths.",
    inputSchema: {
      pid: z.number().int().min(1).describe("Host process ID whose visible application window should be captured."),
      quality: z.number().int().min(1).max(100).default(85).describe("Requested image quality from 1 through 100."),
      include_process_tree: z.boolean().default(true).describe("Also consider visible windows owned by child processes, useful for launchers, browsers, emulators, and multi-process GUI applications."),
    },
    outputSchema,
  };

  server.registerTool(
    "host_capture_display",
    safeReadOnlyTool(displayCaptureTool, `Capturing ${hostTitle.toLowerCase()} display`, `${hostTitle} display captured`),
    captureDisplayHandler,
  );

  server.registerTool(
    "host_capture_window",
    safeReadOnlyTool(windowCaptureTool, `Capturing ${hostTitle.toLowerCase()} window`, `${hostTitle} window captured`),
    captureWindowHandler,
  );

  server.registerTool(
    "host_capture_program",
    safeReadOnlyTool(
      { ...windowCaptureTool, description: "Compatibility alias for host_capture_window." },
      `Capturing ${hostTitle.toLowerCase()} window`,
      `${hostTitle} window captured`,
    ),
    captureWindowHandler,
  );

  server.registerTool(
    "windows_host_capture_display",
    safeReadOnlyTool(
      { ...displayCaptureTool, description: "Compatibility alias for host_capture_display. On Windows this retains the original PR tool name." },
      `Capturing ${hostTitle.toLowerCase()} display`,
      `${hostTitle} display captured`,
    ),
    captureDisplayHandler,
  );

  server.registerTool(
    "windows_host_capture_program",
    safeReadOnlyTool(
      { ...windowCaptureTool, description: "Compatibility alias for host_capture_window. On Windows this retains the original PR tool name." },
      `Capturing ${hostTitle.toLowerCase()} window`,
      `${hostTitle} window captured`,
    ),
    captureWindowHandler,
  );

  server.registerTool(
    "host_exec",
    safeActionTool(
      {
        title: `Run ${hostCommandTitle} Command`,
        description: config.platform.isWindows
          ? `Use this when you explicitly need native ${hostTitle.toLowerCase()} shell automation rather than the ${runtimeLabel}. Prefer host_run_program for a single allowed executable such as git, gh, node, python, or rg because it avoids PowerShell startup overhead. On Windows this runs elevated inside the already-elevated MCP service (no per-command UAC).`
          : `Use this when you explicitly need native ${hostTitle.toLowerCase()} tooling rather than the ${runtimeLabel}, such as shell automation, git, node, python, or other host commands.`,
        inputSchema: {
          command: z.string().min(1).describe(`Command to run on the ${hostTitle.toLowerCase()}.`),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe(`Working directory on the ${hostTitle.toLowerCase()}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
        },
        outputSchema,
      },
      `Running ${hostCommandTitle.toLowerCase()} command`,
      `${hostCommandTitle} command finished`,
    ),
    hostExecHandler,
  );

  server.registerTool(
    "windows_host_inspect_file",
    safeReadOnlyTool(
      {
        title: "Inspect Windows Host File Integrity",
        description:
          "Use this when a Windows host command references a file that may be corrupted, mojibake-encoded, syntactically broken, or otherwise suspect on disk. It inspects exact bytes and PowerShell parser status where relevant.",
        inputSchema: {
          path: z.string().min(1).describe("File path on the Windows host."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe("Working directory used to resolve relative Windows host paths."),
          max_bytes: transferByteCountSchema("Maximum bytes to sample from the start of the file.", 262144),
        },
        outputSchema,
      },
      "Inspecting Windows host file",
      "Windows host file inspected",
    ),
    async ({ path, working_dir: workingDir, max_bytes: maxBytes }) => {
      try {
        const data = await inspectWindowsFile({ path, workingDir, maxBytes });
        return successResult(`Inspected ${path} on the Windows host.`, { data });
      } catch (error) {
        return errorResult(error, `Failed to inspect ${path} on the Windows host.`);
      }
    },
  );

  server.registerTool(
    "windows_host_read_large_file",
    safeReadOnlyTool(
      {
        title: "Read Large Windows Host File Chunk",
        description:
          "Use this when you need an exact byte range from a Windows host file so you can diagnose or repair corruption without lossy UTF-8 conversion.",
        inputSchema: {
          path: z.string().min(1).describe("File path on the Windows host."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe("Working directory used to resolve relative Windows host paths."),
          offset_bytes: z.number().int().min(0).default(0).describe("Starting byte offset within the file."),
          max_bytes: transferByteCountSchema("Maximum raw bytes to return from that offset.", 262144),
        },
        outputSchema,
      },
      "Reading large Windows host file chunk",
      "Large Windows host file chunk read",
    ),
    async ({ path, working_dir: workingDir, offset_bytes: offsetBytes, max_bytes: maxBytes }) => {
      try {
        const data = await readLargeFileOnHost({ path, workingDir, offsetBytes, maxBytes });
        const summary = `Read ${path} from byte ${offsetBytes} on the Windows host.`;
        return successResult(summary, {
          data,
          text: textFromResult(summary, summarizeLargeReadData(data)),
        });
      } catch (error) {
        return errorResult(error, `Failed to read ${path} from byte ${offsetBytes} on the Windows host.`);
      }
    },
  );

  server.registerTool(
    "windows_host_write_large_file",
    safeActionTool(
      {
        title: "Write Large Windows Host File",
        description:
          "Use this when you need to create or overwrite a Windows host file using exact bytes, especially to repair corruption while preserving the intended payload end to end.",
        inputSchema: {
          path: z.string().min(1).describe("File path on the Windows host."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe("Working directory used to resolve relative Windows host paths."),
          content: z.string().optional().describe("Optional UTF-8 text payload to write. Prefer content_base64 for exact byte preservation."),
          content_base64: z.string().optional().describe("Base64-encoded raw bytes to write exactly as provided."),
          append: z.boolean().default(false).describe("Append to the file instead of overwriting it."),
          create_dirs: z.boolean().default(true).describe("Create parent directories if they do not exist."),
          expected_sha256: z.string().regex(/^[A-Fa-f0-9]{64}$/).optional().describe("Optional expected SHA-256 of the decoded payload for end-to-end verification."),
        },
        outputSchema,
      },
      "Writing large Windows host file",
      "Large Windows host file written",
    ),
    async ({
      path,
      working_dir: workingDir,
      content,
      content_base64: contentBase64,
      append,
      create_dirs: createDirs,
      expected_sha256: expectedSha256,
    }) => {
      try {
        const normalizedPayload = normalizeLargeWritePayload({ content, contentBase64 });
        const data = await writeLargeFileOnHost({
          path,
          workingDir,
          contentBase64: normalizedPayload,
          append,
          createDirs,
          expectedSha256,
        });
        const summary = append
          ? `Appended large payload to ${path} on the Windows host and verified the exact bytes.`
          : `Wrote large payload to ${path} on the Windows host and verified the exact bytes.`;
        return successResult(summary, {
          data,
          text: textFromResult(summary, summarizeLargeWriteData(data)),
        });
      } catch (error) {
        return errorResult(error, `Failed to write large payload to ${path} on the Windows host.`);
      }
    },
  );

  server.registerTool(
    "windows_host_exec",
    safeActionTool(
      {
        title: `Run ${hostCommandTitle} Command`,
        description: `Compatibility alias for host_exec. Use this when you explicitly need native ${hostTitle.toLowerCase()} tooling rather than the ${runtimeLabel}.`,
        inputSchema: {
          command: z.string().min(1).describe(`Command to run on the ${hostTitle.toLowerCase()}.`),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe(`Working directory on the ${hostTitle.toLowerCase()}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
        },
        outputSchema,
      },
      `Running ${hostCommandTitle.toLowerCase()} command`,
      `${hostCommandTitle} command finished`,
    ),
    hostExecHandler,
  );

  server.registerTool(
    "host_run_program",
    safeActionTool(
      {
        title: `Run Allowed ${hostTitle} Program`,
        description: `Preferred fast path when you need a specific allowed ${hostTitle.toLowerCase()} program such as git, gh, node, python, or rg with structured arguments; this avoids shell startup and quoting overhead.`,
        inputSchema: {
          program: z.string().min(1).describe("Program name or path. It must be allowed by HOST_PROGRAM_ALLOWLIST."),
          args: z.array(z.string()).default([]).describe("Argument list for the program."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe(`Working directory on the ${hostTitle.toLowerCase()}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
        },
        outputSchema,
      },
      `Running allowed ${hostTitle.toLowerCase()} program`,
      `Allowed ${hostTitle.toLowerCase()} program finished`,
    ),
    hostRunProgramHandler,
  );

  server.registerTool(
    "windows_host_run_program",
    safeActionTool(
      {
        title: `Run Allowed ${hostTitle} Program`,
        description: `Compatibility alias for host_run_program. Use this when you need a specific allowed ${hostTitle.toLowerCase()} program with structured arguments.`,
        inputSchema: {
          program: z.string().min(1).describe("Program name or path. It must be allowed by HOST_PROGRAM_ALLOWLIST."),
          args: z.array(z.string()).default([]).describe("Argument list for the program."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe(`Working directory on the ${hostTitle.toLowerCase()}.`),
          timeout_seconds: syncCommandTimeoutSchema(),
          output_mode: z.enum(["head", "tail", "summary"]).default("tail").describe("Which part of large stdout/stderr to return."),
          max_output_chars: z.number().int().min(100).max(COMMAND_OUTPUT_LIMIT_CHARS).default(COMMAND_OUTPUT_LIMIT_CHARS).describe("Maximum returned characters per stdout/stderr stream."),
          max_output_lines: z.number().int().min(0).max(10000).default(0).describe("Optional maximum returned lines per stream; 0 disables the line limit."),
        },
        outputSchema,
      },
      `Running allowed ${hostTitle.toLowerCase()} program`,
      `Allowed ${hostTitle.toLowerCase()} program finished`,
    ),
    hostRunProgramHandler,
  );

  return server;
};

const isLoopbackAddress = (value) => {
  const rawValue = String(value ?? "").trim();
  if (!rawValue) {
    return false;
  }

  return ["127.0.0.1", "::1", "::ffff:127.0.0.1", "localhost"].includes(rawValue);
};

const isLocalRequest = (req) => {
  if (isLoopbackAddress(req.ip) || isLoopbackAddress(req.hostname)) {
    return true;
  }

  const forwardedFor = String(req.headers["x-forwarded-for"] ?? "").split(",")[0]?.trim();
  return isLoopbackAddress(forwardedFor);
};

const normalizeOrigin = (value) => {
  const rawValue = String(value ?? "").trim();
  if (!rawValue) {
    return "";
  }

  try {
    return new URL(rawValue).origin;
  } catch {
    return "";
  }
};

const appendVary = (existingValue, fieldName) => {
  const fields = String(existingValue ?? "")
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean);

  if (!fields.includes(fieldName)) {
    fields.push(fieldName);
  }

  return fields.join(", ");
};

const shouldExposeGatewayBridge = (req) => config.authMode === "none" && config.enableGatewayBridge && isLocalRequest(req);

const getGatewayBridgeOrigin = (req) => {
  if (!shouldExposeGatewayBridge(req)) {
    return "";
  }

  const origin = normalizeOrigin(req.headers.origin);
  if (!origin) {
    return "";
  }

  return config.gatewayBridgeOrigins.includes(origin) ? origin : "";
};

const resolveLocalBaseUrl = (req) => {
  if (!isLocalRequest(req)) {
    return "";
  }

  const host = String(req.get("host") ?? req.headers.host ?? "").trim();
  if (!host) {
    return "";
  }

  const forwardedProto = String(req.headers["x-forwarded-proto"] ?? "")
    .split(",")[0]
    ?.trim();
  const protocol = forwardedProto || req.protocol || "http";
  return `${protocol}://${host}`.replace(/\/+$/, "");
};

const resolveConnectorBaseUrl = (req) => config.publicBaseUrl || resolveLocalBaseUrl(req);

const applyGatewayBridgeHeaders = (req, res) => {
  const origin = getGatewayBridgeOrigin(req);
  if (!origin) {
    return false;
  }

  const requestHeaders = String(req.headers["access-control-request-headers"] ?? "").trim();
  const allowHeaders = requestHeaders || "authorization, content-type, last-event-id, mcp-protocol-version, mcp-session-id";

  res.setHeader("Access-Control-Allow-Origin", origin);
  res.setHeader("Access-Control-Allow-Methods", "DELETE, GET, HEAD, OPTIONS, POST");
  res.setHeader("Access-Control-Allow-Headers", allowHeaders);
  res.setHeader("Access-Control-Expose-Headers", "mcp-session-id");
  res.setHeader("Access-Control-Max-Age", "600");
  res.setHeader("Vary", appendVary(res.getHeader("Vary"), "Origin"));
  res.setHeader("Vary", appendVary(res.getHeader("Vary"), "Access-Control-Request-Method"));
  res.setHeader("Vary", appendVary(res.getHeader("Vary"), "Access-Control-Request-Headers"));
  res.setHeader("Vary", appendVary(res.getHeader("Vary"), "Access-Control-Request-Private-Network"));

  if (String(req.headers["access-control-request-private-network"] ?? "").toLowerCase() === "true") {
    res.setHeader("Access-Control-Allow-Private-Network", "true");
  }

  return true;
};

const gatewayBridgeInfoForRequest = (req) => ({
  enabled: shouldExposeGatewayBridge(req),
  origins: shouldExposeGatewayBridge(req) ? config.gatewayBridgeOrigins : [],
  private_network_access: shouldExposeGatewayBridge(req),
});

const mcpRequestPeerKey = (req) => createHash("sha256")
  .update(JSON.stringify([
    String(req.headers.authorization ?? ""),
    String(req.headers["mcp-session-id"] ?? ""),
    String(req.headers["x-forwarded-for"] ?? req.socket?.remoteAddress ?? ""),
    String(req.headers["user-agent"] ?? ""),
  ]))
  .digest("hex");

const mcpRequestKey = (req, requestId) =>
  requestId === undefined || requestId === null
    ? ""
    : `${mcpRequestPeerKey(req)}:${typeof requestId}:${String(requestId)}`;

const registerActiveMcpRequest = (key, controller) => {
  if (!key) {
    return;
  }
  const controllers = activeMcpRequestControllers.get(key) ?? new Set();
  controllers.add(controller);
  activeMcpRequestControllers.set(key, controllers);
};

const unregisterActiveMcpRequest = (key, controller) => {
  const controllers = activeMcpRequestControllers.get(key);
  if (!controllers) {
    return;
  }
  controllers.delete(controller);
  if (controllers.size === 0) {
    activeMcpRequestControllers.delete(key);
  }
};

const applyMcpCancellationNotification = (req) => {
  if (req.body?.method !== "notifications/cancelled") {
    return 0;
  }
  const key = mcpRequestKey(req, req.body?.params?.requestId);
  const controllers = activeMcpRequestControllers.get(key);
  if (!controllers) {
    return 0;
  }
  const reason = new Error(String(req.body?.params?.reason || "MCP client cancelled the request."));
  let cancelled = 0;
  for (const controller of controllers) {
    if (!controller.signal.aborted) {
      controller.abort(reason);
      cancelled += 1;
    }
  }
  return cancelled;
};

const allowedHosts = new Set(["127.0.0.1", "localhost", "[::1]"]);
if (config.publicBaseUrl) {
  allowedHosts.add(new URL(config.publicBaseUrl).hostname);
}

const createBoundedMcpExpressApp = ({ host, allowedHosts, jsonBodyLimit }) => {
  const app = express();
  app.use(express.json({ limit: jsonBodyLimit }));

  if (allowedHosts) {
    app.use(hostHeaderValidation(allowedHosts));
  } else if (["127.0.0.1", "localhost", "::1"].includes(host)) {
    app.use(localhostHostValidation());
  } else if (host === "0.0.0.0" || host === "::") {
    console.warn(
      `Warning: Server is binding to ${host} without DNS rebinding protection. ` +
        "Consider using allowedHosts or authentication to protect your server.",
    );
  }

  return app;
};

const app = createBoundedMcpExpressApp({
  host: config.host,
  allowedHosts: [...allowedHosts],
  jsonBodyLimit: config.mcpJsonBodyLimit,
});
app.set("trust proxy", 1);
app.use((req, res, next) => {
  const requestId = randomUUID();
  const startedAt = new Date().toISOString();
  const started = Date.now();
  let requestLogged = false;
  const logRequest = (outcome) => {
    if (requestLogged) {
      return;
    }
    requestLogged = true;
    void appendJsonlEvent(httpUsageLogPath, {
      type: "http_request",
      request_id: requestId,
      started_at: startedAt,
      finished_at: new Date().toISOString(),
      duration_ms: Date.now() - started,
      method: req.method,
      path: req.path,
      status_code: outcome === "finished" ? res.statusCode : null,
      outcome,
      client_aborted: outcome === "client_aborted",
      accept: String(req.headers.accept ?? ""),
      user_agent: String(req.headers["user-agent"] ?? ""),
      forwarded_for: String(req.headers["x-forwarded-for"] ?? "").split(",")[0]?.trim() || null,
    }).catch(() => {});
  };
  res.once("finish", () => logRequest("finished"));
  res.once("close", () => {
    if (!res.writableEnded) {
      logRequest("client_aborted");
    }
  });

  const origin = normalizeOrigin(req.headers.origin);
  const bridgeAllowed = applyGatewayBridgeHeaders(req, res);

  if (req.method === "OPTIONS" && origin) {
    if (!shouldExposeGatewayBridge(req)) {
      res.status(405).end();
      return;
    }

    if (!bridgeAllowed) {
      res.status(403).json({
        error: "Origin is not allowed for the local gateway bridge.",
      });
      return;
    }

    res.status(204).end();
    return;
  }

  next();
});

let authMiddleware = null;
let legacyAuthMiddleware = null;
let oauthInfo = null;
let legacyProtectedResourceMetadata = null;

const acceptsEventStream = (req) => String(req.headers.accept ?? "").includes("text/event-stream");

if (config.authMode === "demo-oauth" || config.authMode === "cloudflare-access") {
  const provider =
    config.authMode === "cloudflare-access"
      ? new CloudflareAccessOAuthProvider({
          teamDomain: config.cloudflareAccessTeamDomain,
          audience: config.cloudflareAccessAud,
          jwksUrl: config.cloudflareAccessJwksUrl,
          stateFilePath: config.oauthStateFilePath,
        })
      : new DemoOAuthProvider({
          stateFilePath: config.oauthStateFilePath,
        });
  const issuerUrl = new URL(config.publicBaseUrl);
  const rootMcpServerUrl = new URL("/", config.publicBaseUrl);
  const legacyMcpServerUrl = new URL("/mcp", config.publicBaseUrl);

  app.use(
    mcpAuthRouter({
      provider,
      issuerUrl,
      resourceServerUrl: rootMcpServerUrl,
      scopesSupported: ["mcp:tools"],
      resourceName: runtimeServerName,
    }),
  );

  authMiddleware = requireBearerAuth({
    verifier: provider,
    requiredScopes: [],
    resourceMetadataUrl: getOAuthProtectedResourceMetadataUrl(rootMcpServerUrl),
  });

  legacyAuthMiddleware = requireBearerAuth({
    verifier: provider,
    requiredScopes: [],
    resourceMetadataUrl: getOAuthProtectedResourceMetadataUrl(legacyMcpServerUrl),
  });

  oauthInfo = {
    issuer: issuerUrl.toString(),
    resourceMetadataUrl: getOAuthProtectedResourceMetadataUrl(rootMcpServerUrl),
    legacyResourceMetadataUrl: getOAuthProtectedResourceMetadataUrl(legacyMcpServerUrl),
  };

  legacyProtectedResourceMetadata = {
    resource: legacyMcpServerUrl.toString(),
    authorization_servers: [issuerUrl.toString()],
    scopes_supported: ["mcp:tools"],
    resource_name: "Docker ChatGPT Devbox MCP",
  };
}

app.get("/", async (_req, res) => {
  if (acceptsEventStream(_req)) {
    handleMcpSseProbe(_req, res);
    return;
  }

  const connectorBaseUrl = resolveConnectorBaseUrl(_req);
  const localBaseUrl = resolveLocalBaseUrl(_req);
  const baseResponse = {
    name: runtimeServerName,
    version,
    auth_mode: config.authMode,
    runtime_mode: config.runtimeMode,
    platform: config.platform.id,
    public_base_url: config.publicBaseUrl || null,
    local_base_url: localBaseUrl || null,
    mcp_url: connectorBaseUrl ? `${connectorBaseUrl}/mcp` : null,
    root_mcp_url: connectorBaseUrl || null,
    gateway_bridge: gatewayBridgeInfoForRequest(_req),
    oauth: oauthInfo,
    notes:
      config.authMode === "cloudflare-access"
        ? "Cloudflare Access-backed OAuth is enabled for ChatGPT app testing. Protect the /authorize path with a Cloudflare Access application."
        : config.authMode === "demo-oauth"
          ? `Demo OAuth is enabled for ChatGPT app testing. The ${runtimeLabel} is the main execution environment; host tools are separate and explicit.`
          : `No authentication mode is active. The ${runtimeLabel} is the main execution environment; host tools are separate and explicit.`,
  };

  if (config.authMode !== "none" && !isLocalRequest(_req)) {
    res.json(baseResponse);
    return;
  }

  const devbox = await getDevboxInfo().catch((error) => ({
    exists: false,
    running: false,
    status: `error: ${error instanceof Error ? error.message : "unknown"}`,
  }));

  res.json({
    ...baseResponse,
    runtime: {
      runtimeMode: config.runtimeMode,
      platform: config.platform.id,
      hostShell: config.hostShell,
      devboxContainerName: config.devboxContainerName,
      devboxImageName: config.devboxImageName,
      devboxWorkspacePath: config.devboxWorkspacePath,
      hostWorkspacePath: config.hostWorkspacePath,
      hostExecEnabled: config.enableHostExec,
    },
    devbox,
  });
});

app.get("/healthz", (_req, res) => {
  res.type("text/plain").send("ok");
});
app.get("/livez", (_req, res) => {
  res.type("text/plain").send("ok");
});
app.get("/readyz", async (_req, res) => {
  try {
    await jobsRootReady;
    await executionStoreInitialProbe.catch(() => {});
    const ageMs = Date.now() - Number(executionStoreHealth.sampledAtMs || 0);
    const ready = executionStoreHealth.ok === true && ageMs < EXECUTION_STORE_PROBE_STALE_MS;
    res.status(ready ? 200 : 503).json({ ok: ready });
  } catch (error) {
    console.warn("Readiness probe failed", error);
    res.status(503).json({ ok: false });
  }
});

if (legacyProtectedResourceMetadata) {
  app.get("/.well-known/oauth-protected-resource/mcp", (_req, res) => {
    res.json(legacyProtectedResourceMetadata);
  });
}

const handleMcpRequest = async (req, res) => {
  applyMcpCancellationNotification(req);
  const requestAbortController = new AbortController();
  const activeRequestKey = mcpRequestKey(req, req.body?.id);
  registerActiveMcpRequest(activeRequestKey, requestAbortController);
  const server = buildServer({ requestSignal: requestAbortController.signal });
  let settleResponse;
  const responseSettled = new Promise((resolve) => {
    settleResponse = resolve;
  });
  const markResponseSettled = () => settleResponse();
  const abortDisconnectedRequest = () => {
    if (!res.writableEnded && !requestAbortController.signal.aborted) {
      requestAbortController.abort(new Error("MCP HTTP client disconnected before the tool result was delivered."));
    }
  };
  req.once("aborted", abortDisconnectedRequest);
  res.once("close", abortDisconnectedRequest);
  res.once("finish", markResponseSettled);
  res.once("close", markResponseSettled);

  try {
    const transport = new StreamableHTTPServerTransport({
      sessionIdGenerator: undefined,
    });

    await server.connect(transport);
    await transport.handleRequest(req, res, req.body);
    await responseSettled;
  } catch (error) {
    if (!res.headersSent && !res.destroyed) {
      res.status(500).json({
        jsonrpc: "2.0",
        error: {
          code: -32603,
          message: error instanceof Error ? error.message : "Internal server error",
        },
        id: req.body?.id ?? null,
      });
    }
    if (!res.destroyed) {
      await responseSettled;
    }
  } finally {
    unregisterActiveMcpRequest(activeRequestKey, requestAbortController);
    req.removeListener("aborted", abortDisconnectedRequest);
    res.removeListener("close", abortDisconnectedRequest);
    res.removeListener("finish", markResponseSettled);
    res.removeListener("close", markResponseSettled);
    await server.close().catch(() => {});
  }
};

const rootProtectedMcpHandlers = config.authMode !== "none" && authMiddleware ? [authMiddleware, handleMcpRequest] : [handleMcpRequest];
const legacyProtectedMcpHandlers =
  config.authMode !== "none" && legacyAuthMiddleware ? [legacyAuthMiddleware, handleMcpRequest] : [handleMcpRequest];

const handleMcpSseProbe = (req, res) => {
  const acceptHeader = String(req.headers.accept ?? "");
  if (!acceptHeader.includes("text/event-stream")) {
    res.status(406).json({
      jsonrpc: "2.0",
      error: {
        code: -32000,
        message: "Not Acceptable: Client must accept text/event-stream",
      },
      id: null,
    });
    return;
  }

  res.status(200);
  res.set({
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache, no-transform",
    Connection: "keep-alive",
    "X-Accel-Buffering": "no",
  });

  if (typeof res.flushHeaders === "function") {
    res.flushHeaders();
  }

  // Emit an immediate SSE comment so proxy layers flush the stream headers.
  res.write(": mcp-sse-probe\n\n");

  const closeTimer = setTimeout(() => {
    if (!res.writableEnded) {
      res.end();
    }
  }, 1000);

  req.on("close", () => {
    clearTimeout(closeTimer);
  });
};

app.post("/", ...rootProtectedMcpHandlers);
app.delete("/", ...rootProtectedMcpHandlers);
app.post("/mcp", ...legacyProtectedMcpHandlers);
app.get("/mcp", handleMcpSseProbe);
app.delete("/mcp", ...legacyProtectedMcpHandlers);

export { app };

export const startServer = () => {
  let orphanReconcileTimer = null;
  const httpServer = app.listen(config.port, config.host, () => {
    console.log(`${runtimeServerName} listening on ${config.host}:${config.port}`);
    console.log(`Runtime mode: ${config.runtimeMode} (${config.platform.displayName})`);
    console.log(`Auth mode: ${config.authMode}`);
    if (config.publicBaseUrl) {
      console.log(`Public MCP URL: ${config.publicBaseUrl}/mcp`);
    }
    warmHostExecutionState().catch(() => {});
    getDevboxVersions().catch(() => {});
    reconcileOrphanedDevboxJobs().catch(() => {});
    const scheduleOrphanReconcile = (delayMs) => {
      orphanReconcileTimer = setTimeout(async () => {
        await reconcileOrphanedDevboxJobs().catch(() => {});
        scheduleOrphanReconcile(60000);
      }, delayMs);
      orphanReconcileTimer.unref?.();
    };
    scheduleOrphanReconcile(17000);
  });
  httpServer.once("close", () => {
    if (orphanReconcileTimer) clearTimeout(orphanReconcileTimer);
    orphanReconcileTimer = null;
  });
  return httpServer;
};

const isMainModule = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);

if (isMainModule) {
  startServer();
}
