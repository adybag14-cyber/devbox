import { readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const defaultProjectRoot = path.resolve(__dirname, "..");

const parseArgs = (argv) => {
  const args = {};

  for (let index = 0; index < argv.length; index += 1) {
    const entry = argv[index];
    if (!entry.startsWith("--")) {
      continue;
    }

    const key = entry.slice(2);
    const next = argv[index + 1];
    if (!next || next.startsWith("--")) {
      args[key] = true;
      continue;
    }

    args[key] = next;
    index += 1;
  }

  return args;
};

const readJsonlFile = async (filePath) => {
  try {
    const raw = await readFile(filePath, "utf8");
    return raw
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => JSON.parse(line));
  } catch (error) {
    if (error?.code === "ENOENT") {
      return [];
    }
    throw error;
  }
};

const readJsonFile = async (filePath) => {
  try {
    return JSON.parse(await readFile(filePath, "utf8"));
  } catch (error) {
    if (error?.code === "ENOENT") {
      return null;
    }
    throw error;
  }
};

const toEpochMs = (value) => {
  const timestamp = Date.parse(String(value ?? ""));
  return Number.isFinite(timestamp) ? timestamp : null;
};

const truncateString = (value, maxLength = 240) =>
  typeof value === "string" && value.length > maxLength ? `${value.slice(0, maxLength)}...` : value;

const eventTimestampMs = (event) =>
  toEpochMs(event.finished_at) ?? toEpochMs(event.started_at) ?? toEpochMs(event.timestamp) ?? null;

const summarizeToolEvents = (events, slowMs) => {
  const finishEvents = events.filter((event) => event.type === "tool_finish" || event.type === "tool_throw");
  const perTool = new Map();
  const timeoutSuspects = [];
  const slowEvents = [];

  for (const event of finishEvents) {
    const toolName = event.tool ?? "unknown";
    const entry =
      perTool.get(toolName) ??
      {
        tool: toolName,
        calls: 0,
        success: 0,
        error: 0,
        throws: 0,
        max_duration_ms: 0,
        avg_duration_ms: 0,
        durations: [],
        last_summary: null,
        last_error: null,
      };

    entry.calls += 1;
    const durationMs = Number(event.duration_ms) || 0;
    entry.max_duration_ms = Math.max(entry.max_duration_ms, durationMs);
    entry.durations.push(durationMs);

    if (event.type === "tool_throw") {
      entry.throws += 1;
      entry.last_error = truncateString(event.error ?? "tool_throw");
    } else if (event.ok) {
      entry.success += 1;
      entry.last_summary = truncateString(event.summary ?? null);
    } else {
      entry.error += 1;
      entry.last_error = truncateString(event.summary ?? "tool error");
    }

    const timeoutText = `${event.summary ?? ""} ${event.error ?? ""}`;
    if (/timed?\s*out|timeout/i.test(timeoutText)) {
      timeoutSuspects.push({
        tool: toolName,
        duration_ms: durationMs,
        summary: truncateString(event.summary ?? null),
        error: truncateString(event.error ?? null),
        context: event.context ?? null,
      });
    }

    if (durationMs >= slowMs) {
      slowEvents.push({
        tool: toolName,
        duration_ms: durationMs,
        ok: event.type === "tool_finish" ? Boolean(event.ok) : false,
        summary: truncateString(event.summary ?? null),
        error: truncateString(event.error ?? null),
        context: event.context ?? null,
      });
    }

    perTool.set(toolName, entry);
  }

  const perToolSummary = [...perTool.values()]
    .map((entry) => ({
      tool: entry.tool,
      calls: entry.calls,
      success: entry.success,
      error: entry.error,
      throws: entry.throws,
      max_duration_ms: entry.max_duration_ms,
      avg_duration_ms: entry.durations.length
        ? Math.round(entry.durations.reduce((sum, value) => sum + value, 0) / entry.durations.length)
        : 0,
      last_summary: entry.last_summary,
      last_error: entry.last_error,
    }))
    .sort((left, right) => right.calls - left.calls || left.tool.localeCompare(right.tool));

  return {
    total_events: finishEvents.length,
    total_success: perToolSummary.reduce((sum, entry) => sum + entry.success, 0),
    total_errors: perToolSummary.reduce((sum, entry) => sum + entry.error + entry.throws, 0),
    per_tool: perToolSummary,
    timeout_suspects: timeoutSuspects,
    slow_events: slowEvents.sort((left, right) => right.duration_ms - left.duration_ms),
  };
};

const summarizeHttpEvents = (events, slowMs) => {
  const statusBuckets = {};
  const errorResponses = [];
  const slowRequests = [];

  for (const event of events) {
    const statusCode = Number(event.status_code) || 0;
    const bucketKey = String(statusCode);
    statusBuckets[bucketKey] = (statusBuckets[bucketKey] ?? 0) + 1;

    if (statusCode >= 400) {
      errorResponses.push({
        method: event.method ?? null,
        path: event.path ?? null,
        status_code: statusCode,
        duration_ms: Number(event.duration_ms) || 0,
        user_agent: truncateString(event.user_agent ?? null),
      });
    }

    if ((Number(event.duration_ms) || 0) >= slowMs) {
      slowRequests.push({
        method: event.method ?? null,
        path: event.path ?? null,
        status_code: statusCode,
        duration_ms: Number(event.duration_ms) || 0,
        user_agent: truncateString(event.user_agent ?? null),
      });
    }
  }

  return {
    total_requests: events.length,
    status_buckets: statusBuckets,
    error_responses: errorResponses,
    slow_requests: slowRequests.sort((left, right) => right.duration_ms - left.duration_ms),
  };
};

export const summarizeTelemetry = async ({
  projectRoot = defaultProjectRoot,
  since = null,
  slowMs = 10000,
} = {}) => {
  const runDir = path.join(projectRoot, "run");
  const toolEvents = await readJsonlFile(path.join(runDir, "tool-usage.jsonl"));
  const httpEvents = await readJsonlFile(path.join(runDir, "http-usage.jsonl"));
  const guardianState = await readJsonFile(path.join(runDir, "guardian", "state.json"));
  const sinceMs = since ? toEpochMs(since) : null;

  const filteredToolEvents = sinceMs ? toolEvents.filter((event) => (eventTimestampMs(event) ?? 0) >= sinceMs) : toolEvents;
  const filteredHttpEvents = sinceMs ? httpEvents.filter((event) => (eventTimestampMs(event) ?? 0) >= sinceMs) : httpEvents;

  return {
    generated_at: new Date().toISOString(),
    project_root: projectRoot,
    since: sinceMs ? new Date(sinceMs).toISOString() : null,
    guardian: guardianState
      ? {
          observed_at: guardianState.ObservedAtUtc ?? null,
          is_healthy: Boolean(guardianState.IsHealthy),
          needs_repair: Boolean(guardianState.NeedsRepair),
          docker_ready: Boolean(guardianState.DockerReady),
          devbox_running: Boolean(guardianState.DevboxRunning),
          cloudflared_running: guardianState.CloudflaredRunning ?? null,
          local_health: Boolean(guardianState.LocalHealth),
          public_health: guardianState.PublicHealth ?? null,
          reasons: guardianState.Reasons ?? [],
        }
      : null,
    tools: summarizeToolEvents(filteredToolEvents, slowMs),
    http: summarizeHttpEvents(filteredHttpEvents, slowMs),
  };
};

const isMainModule = process.argv[1] === fileURLToPath(import.meta.url);

if (isMainModule) {
  const args = parseArgs(process.argv.slice(2));
  const projectRoot = args["project-root"] ? path.resolve(args["project-root"]) : defaultProjectRoot;
  const since = typeof args.since === "string" ? args.since : null;
  const slowMs = Number(args["slow-ms"]) > 0 ? Number(args["slow-ms"]) : 10000;
  const summary = await summarizeTelemetry({ projectRoot, since, slowMs });
  process.stdout.write(`${JSON.stringify(summary, null, 2)}\n`);
}
