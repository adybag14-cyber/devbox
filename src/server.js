import { appendFile, mkdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { createHash, randomUUID } from "node:crypto";
import { fileURLToPath } from "node:url";

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
  getDevboxVersions,
  hostCommandTitle,
  hostTitle,
  isDockerRuntime,
  listFilesInDevbox,
  readFileInDevbox,
  readLargeFileInDevbox,
  recreateDevbox,
  restartDevbox,
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
  writeLargeFileOnHost,
} from "./host-tools.js";
import { captureFullDisplayJpeg, captureProgramWindowJpeg } from "./windows-screen-capture.js";
import { CloudflareAccessOAuthProvider, DemoOAuthProvider } from "./oauth.js";
import {
  normalizeLargeWritePayload,
  summarizeLargeReadData,
  summarizeLargeWriteData,
} from "./large-file-cli.js";
import { trimText } from "./process-utils.js";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const runDir = path.join(projectRoot, "run");
const guardianDesiredStatePath = path.join(runDir, "guardian.desired-state.json");
const toolUsageLogPath = path.join(runDir, "tool-usage.jsonl");
const httpUsageLogPath = path.join(runDir, "http-usage.jsonl");
const logRotationChains = new Map();
const activeMcpRequestControllers = new Map();

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
const COMMAND_OUTPUT_LIMIT_CHARS = config.maxTextOutputChars === null
  ? config.maxCommandOutputChars
  : Math.min(config.maxTextOutputChars, config.maxCommandOutputChars);
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

const rotateUsageLogIfNeeded = async (logPath) => {
  const maxBytes = config.mcpUsageLogMaxBytes;
  const rotations = Math.max(0, config.mcpUsageLogRotations);
  if (!Number.isFinite(maxBytes) || maxBytes <= 0 || rotations <= 0) {
    return;
  }

  let currentStat;
  try {
    currentStat = await stat(logPath);
  } catch (error) {
    if (error?.code === "ENOENT") {
      return;
    }
    throw error;
  }

  if (currentStat.size < maxBytes) {
    return;
  }

  await rm(`${logPath}.${rotations}`, { force: true });
  for (let index = rotations - 1; index >= 1; index -= 1) {
    await rename(`${logPath}.${index}`, `${logPath}.${index + 1}`).catch((error) => {
      if (error?.code !== "ENOENT") {
        throw error;
      }
    });
  }
  await rename(logPath, `${logPath}.1`).catch((error) => {
    if (error?.code !== "ENOENT") {
      throw error;
    }
  });
};

const appendJsonlEvent = async (logPath, event) => {
  await mkdir(path.dirname(logPath), { recursive: true });
  const previous = logRotationChains.get(logPath) ?? Promise.resolve();
  const next = previous
    .catch(() => {})
    .then(async () => {
      await rotateUsageLogIfNeeded(logPath);
      await appendFile(logPath, `${JSON.stringify(event)}\n`, "utf8");
    });

  logRotationChains.set(logPath, next);
  try {
    await next;
  } finally {
    if (logRotationChains.get(logPath) === next) {
      logRotationChains.delete(logPath);
    }
  }
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

const jpegImageResult = (summary, { jpeg, metadata }) => {
  const result = successResult(summary, {
    data: metadata,
    text: textFromResult(summary, metadata),
  });
  result.content.push({
    type: "image",
    data: jpeg.toString("base64"),
    mimeType: "image/jpeg",
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
  const stdout = trimText(result.stdout, COMMAND_OUTPUT_LIMIT_CHARS);
  const stderr = trimText(result.stderr, COMMAND_OUTPUT_LIMIT_CHARS);

  return successResult(summary, {
    data: extra.data,
    stdout: stdout.text || undefined,
    stderr: stderr.text || undefined,
    exitCode: result.exitCode ?? null,
    truncated: stdout.truncated || stderr.truncated,
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
        };

        if (info.running) {
          data.versions = await getDevboxVersions();
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
          ? "Prefer this for inspection-only shell work such as ls, find, cat, sed -n, rg, git diff, git log, or config inspection. It runs in the long-lived devbox container; read-only behavior is advisory so Docker clients do not strand during disposable-container probes."
          : `Prefer this for inspection-only shell work such as ls, find, cat, sed -n, rg, git diff, git log, or config inspection. In ${runtimeLabel} mode this runs directly on the host shell, so read-only behavior is advisory rather than sandbox-enforced.`,
        inputSchema: {
          command: z.string().min(1).describe(`Read-only shell command to run inside the ${runtimeLabel}.`),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: z.number().int().min(1).max(7200).default(900).describe("Command timeout in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the disposable read-only container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running read-only shell command in ${runtimeLabel}`,
      `Read-only ${runtimeLabel} shell command finished`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user }, extra) => {
      try {
        const result = await execReadOnlyInDevbox({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          user,
          signal: extra?.signal,
        });
        return fromProcessResult(`Ran a read-only shell command in the ${runtimeLabel} at ${workingDir}.`, result);
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
          ? "Use this only when the shell command needs side effects such as writing files, building artifacts, installing packages, changing git state, or otherwise mutating the devbox or workspace. Prefer devbox_exec_readonly for inspection, search, and file-reading commands."
          : `Use this when the shell command needs side effects such as writing files, building artifacts, installing packages, changing git state, or otherwise mutating the ${runtimeLabel}. Prefer devbox_exec_readonly for inspection, search, and file-reading commands.`,
        inputSchema: {
          command: z.string().min(1).describe(`Shell command to run inside the ${runtimeLabel}.`),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: z.number().int().min(1).max(7200).default(900).describe("Command timeout in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the devbox container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running shell command in ${runtimeLabel}`,
      `${runtimeLabel} shell command finished`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user }, extra) => {
      try {
        const result = await execInDevbox({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          user,
          signal: extra?.signal,
        });
        return fromProcessResult(`Ran a shell command in the ${runtimeLabel} at ${workingDir}.`, result);
      } catch (error) {
        return errorResult(error, `Failed to run the ${runtimeLabel} shell command.`);
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
    }, extra) => {
      try {
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
          signal: extra?.signal,
        });
        return fromProcessResult(`Searched ${path} for "${pattern}" inside the ${runtimeLabel}.`, result);
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

  const hostExecHandler = async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds }, extra) => {
    try {
      const result = await runHostShellCommand({
        command,
        workingDir,
        timeoutMs: (timeoutSeconds + 5) * 1000,
        signal: extra?.signal,
      });
      return fromProcessResult(`Ran a ${hostCommandTitle.toLowerCase()} command in ${workingDir}.`, result);
    } catch (error) {
      return errorResult(error, `Failed to run the ${hostCommandTitle.toLowerCase()} command.`);
    }
  };

  const hostRunProgramHandler = async ({ program, args, working_dir: workingDir, timeout_seconds: timeoutSeconds }, extra) => {
    try {
      const result = await runAllowedProgram({
        program,
        args,
        workingDir,
        timeoutMs: (timeoutSeconds + 5) * 1000,
        signal: extra?.signal,
      });
      return fromProcessResult(`Ran ${program} on the ${hostTitle.toLowerCase()}.`, result);
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

  server.registerTool(
    "windows_host_capture_display",
    safeReadOnlyTool(
      {
        title: "Capture Full Windows Display as JPEG",
        description:
          "Capture the complete Windows virtual desktop, including all attached displays, and return the actual screenshot as an image/jpeg MCP content block.",
        inputSchema: {
          quality: z.number().int().min(1).max(100).default(85).describe("JPEG quality from 1 through 100."),
        },
        outputSchema,
      },
      "Capturing full Windows display",
      "Full Windows display captured",
    ),
    async ({ quality }) => {
      try {
        const capture = await captureFullDisplayJpeg({ quality });
        return jpegImageResult("Captured the full Windows display as JPEG.", capture);
      } catch (error) {
        return errorResult(error, "Failed to capture the full Windows display.");
      }
    },
  );

  server.registerTool(
    "windows_host_capture_program",
    safeReadOnlyTool(
      {
        title: "Capture Windows Program by PID as JPEG",
        description:
          "Capture the largest visible, non-minimized top-level window owned by a specific Windows host process ID and return the actual screenshot as an image/jpeg MCP content block.",
        inputSchema: {
          pid: z.number().int().min(1).describe("Windows host process ID whose visible program window should be captured."),
          quality: z.number().int().min(1).max(100).default(85).describe("JPEG quality from 1 through 100."),
        },
        outputSchema,
      },
      "Capturing Windows program window",
      "Windows program window captured",
    ),
    async ({ pid, quality }) => {
      try {
        const capture = await captureProgramWindowJpeg({ pid, quality });
        return jpegImageResult(`Captured Windows program PID ${pid} as JPEG.`, capture);
      } catch (error) {
        return errorResult(error, `Failed to capture Windows program PID ${pid}.`);
      }
    },
  );

  server.registerTool(
    "host_exec",
    safeActionTool(
      {
        title: `Run ${hostCommandTitle} Command`,
        description: config.platform.isWindows
          ? `Use this when you explicitly need native ${hostTitle.toLowerCase()} tooling rather than the ${runtimeLabel}, such as winget, host Git, host Docker CLI, or PowerShell automation. This may prompt for elevation on Windows when needed.`
          : `Use this when you explicitly need native ${hostTitle.toLowerCase()} tooling rather than the ${runtimeLabel}, such as shell automation, git, node, python, or other host commands.`,
        inputSchema: {
          command: z.string().min(1).describe(`Command to run on the ${hostTitle.toLowerCase()}.`),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe(`Working directory on the ${hostTitle.toLowerCase()}.`),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
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
          timeout_seconds: z.number().int().min(1).max(7200).default(900).describe("Command timeout in seconds."),
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
        description: `Use this when you need a specific allowed ${hostTitle.toLowerCase()} program such as git, node, python, gh, or other tools from HOST_PROGRAM_ALLOWLIST with structured arguments.`,
        inputSchema: {
          program: z.string().min(1).describe("Program name or path. It must be allowed by HOST_PROGRAM_ALLOWLIST."),
          args: z.array(z.string()).default([]).describe("Argument list for the program."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe(`Working directory on the ${hostTitle.toLowerCase()}.`),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
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
          timeout_seconds: z.number().int().min(1).max(7200).default(900).describe("Command timeout in seconds."),
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

export const startServer = () =>
  app.listen(config.port, config.host, () => {
    console.log(`${runtimeServerName} listening on ${config.host}:${config.port}`);
    console.log(`Runtime mode: ${config.runtimeMode} (${config.platform.displayName})`);
    console.log(`Auth mode: ${config.authMode}`);
    if (config.publicBaseUrl) {
      console.log(`Public MCP URL: ${config.publicBaseUrl}/mcp`);
    }
  });

const isMainModule = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);

if (isMainModule) {
  startServer();
}
