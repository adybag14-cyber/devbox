import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { createMcpExpressApp } from "@modelcontextprotocol/sdk/server/express.js";
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
  runAllowedProgram,
  runHostShellCommand,
} from "./host-tools.js";
import { CloudflareAccessOAuthProvider, DemoOAuthProvider } from "./oauth.js";
import {
  normalizeLargeWritePayload,
  summarizeLargeReadData,
  summarizeLargeWriteData,
} from "./large-file-cli.js";
import { trimText } from "./process-utils.js";

const outputSchema = {
  ok: z.boolean(),
  summary: z.string(),
  data: z.any().optional(),
  stdout: z.string().optional(),
  stderr: z.string().optional(),
  exitCode: z.number().nullable().optional(),
  truncated: z.boolean().optional(),
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
  const structuredContent = {
    ok: true,
    summary,
    data: extra.data,
    stdout: extra.stdout,
    stderr: extra.stderr,
    exitCode: extra.exitCode ?? null,
    truncated: extra.truncated ?? false,
  };

  return {
    content: [
      {
        type: "text",
        text: extra.text ?? textFromResult(summary, extra.data, extra.stdout, extra.stderr),
      },
    ],
    structuredContent,
  };
};

const isCommandStyleError = (error) =>
  Boolean(
    error &&
      typeof error === "object" &&
      (error instanceof HostCommandError || "exitCode" in error || "stdout" in error || "stderr" in error),
  );

const errorResult = (error, fallbackSummary = "The command failed.") => {
  if (isCommandStyleError(error)) {
    const stdout = trimText(error.stdout, config.maxTextOutputChars);
    const stderr = trimText(error.stderr, config.maxTextOutputChars);
    const summary = error.message || fallbackSummary;

    return {
      content: [
        {
          type: "text",
          text: textFromResult(summary, undefined, stdout.text, stderr.text),
        },
      ],
      structuredContent: {
        ok: false,
        summary,
        stdout: stdout.text,
        stderr: stderr.text,
        exitCode: error.exitCode ?? null,
        truncated: stdout.truncated || stderr.truncated,
      },
      isError: true,
    };
  }

  return {
    content: [
      {
        type: "text",
        text: error instanceof Error ? error.message : fallbackSummary,
      },
    ],
    structuredContent: {
      ok: false,
      summary: error instanceof Error ? error.message : fallbackSummary,
      exitCode: null,
      truncated: false,
    },
    isError: true,
  };
};

const fromProcessResult = (summary, result, extra = {}) => {
  const stdout = trimText(result.stdout, config.maxTextOutputChars);
  const stderr = trimText(result.stderr, config.maxTextOutputChars);

  return successResult(summary, {
    data: extra.data,
    stdout: stdout.text || undefined,
    stderr: stderr.text || undefined,
    exitCode: result.exitCode ?? null,
    truncated: stdout.truncated || stderr.truncated,
  });
};

const buildServer = () => {
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
          ? "Prefer this for inspection-only shell work such as ls, find, cat, sed -n, rg, git diff, git log, or config inspection. It runs in a disposable container with the workspace mounted read-only and network disabled, so project writes and outbound network access should fail."
          : `Prefer this for inspection-only shell work such as ls, find, cat, sed -n, rg, git diff, git log, or config inspection. In ${runtimeLabel} mode this runs directly on the host shell, so read-only behavior is advisory rather than sandbox-enforced.`,
        inputSchema: {
          command: z.string().min(1).describe(`Read-only shell command to run inside the ${runtimeLabel}.`),
          working_dir: z.string().default(config.devboxWorkspacePath).describe(`Working directory inside the ${runtimeLabel}.`),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the disposable read-only container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running read-only shell command in ${runtimeLabel}`,
      `Read-only ${runtimeLabel} shell command finished`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user }) => {
      try {
        const result = await execReadOnlyInDevbox({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          user,
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
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe(isDockerRuntime ? "Linux user inside the devbox container." : `Host user hint for ${runtimeLabel} mode.`),
        },
        outputSchema,
      },
      `Running shell command in ${runtimeLabel}`,
      `${runtimeLabel} shell command finished`,
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user }) => {
      try {
        const result = await execInDevbox({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          user,
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
        },
        outputSchema,
      },
      `Listing ${runtimeLabel} files`,
      `${runtimeLabel} files listed`,
    ),
    async ({ path, recursive, max_depth: maxDepth }) => {
      try {
        const result = await listFilesInDevbox({ path, recursive, maxDepth });
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
          max_bytes: z.number().int().min(1).max(500000).default(65536).describe("Maximum bytes to return."),
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
          max_bytes: z.number().int().min(1).max(524288).default(262144).describe("Maximum raw bytes to return from that offset."),
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
        },
        outputSchema,
      },
      `Searching ${runtimeLabel} files`,
      `${runtimeLabel} search finished`,
    ),
    async ({ pattern, path, glob, case_sensitive: caseSensitive, max_matches: maxMatches }) => {
      try {
        const result = await searchFilesInDevbox({ pattern, path, glob, caseSensitive, maxMatches });
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

  const hostExecHandler = async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds }) => {
    try {
      const result = await runHostShellCommand({
        command,
        workingDir,
        timeoutMs: (timeoutSeconds + 5) * 1000,
      });
      return fromProcessResult(`Ran a ${hostCommandTitle.toLowerCase()} command in ${workingDir}.`, result);
    } catch (error) {
      return errorResult(error, `Failed to run the ${hostCommandTitle.toLowerCase()} command.`);
    }
  };

  const hostRunProgramHandler = async ({ program, args, working_dir: workingDir, timeout_seconds: timeoutSeconds }) => {
    try {
      const result = await runAllowedProgram({
        program,
        args,
        workingDir,
        timeoutMs: (timeoutSeconds + 5) * 1000,
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
    "windows_host_exec",
    safeActionTool(
      {
        title: `Run ${hostCommandTitle} Command`,
        description: `Compatibility alias for host_exec. Use this when you explicitly need native ${hostTitle.toLowerCase()} tooling rather than the ${runtimeLabel}.`,
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
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
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

const allowedHosts = new Set(["127.0.0.1", "localhost", "[::1]"]);
if (config.publicBaseUrl) {
  allowedHosts.add(new URL(config.publicBaseUrl).hostname);
}

const app = createMcpExpressApp({
  host: config.host,
  allowedHosts: [...allowedHosts],
});
app.set("trust proxy", 1);

let authMiddleware = null;
let oauthInfo = null;

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
  const mcpServerUrl = new URL("/mcp", config.publicBaseUrl);

  app.use(
    mcpAuthRouter({
      provider,
      issuerUrl,
      resourceServerUrl: mcpServerUrl,
      scopesSupported: ["mcp:tools"],
      resourceName: runtimeServerName,
    }),
  );

  authMiddleware = requireBearerAuth({
    verifier: provider,
    requiredScopes: [],
    resourceMetadataUrl: getOAuthProtectedResourceMetadataUrl(mcpServerUrl),
  });

  oauthInfo = {
    issuer: issuerUrl.toString(),
    resourceMetadataUrl: getOAuthProtectedResourceMetadataUrl(mcpServerUrl),
  };
}

app.get("/", async (_req, res) => {
  const baseResponse = {
    name: runtimeServerName,
    version,
    auth_mode: config.authMode,
    runtime_mode: config.runtimeMode,
    platform: config.platform.id,
    public_base_url: config.publicBaseUrl || null,
    mcp_url: config.publicBaseUrl ? `${config.publicBaseUrl}/mcp` : null,
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

const handleMcpPost = async (req, res) => {
  const server = buildServer();

  try {
    const transport = new StreamableHTTPServerTransport({
      sessionIdGenerator: undefined,
    });

    await server.connect(transport);
    await transport.handleRequest(req, res, req.body);

    res.on("close", () => {
      transport.close().catch(() => {});
      server.close().catch(() => {});
    });
  } catch (error) {
    if (!res.headersSent) {
      res.status(500).json({
        jsonrpc: "2.0",
        error: {
          code: -32603,
          message: error instanceof Error ? error.message : "Internal server error",
        },
        id: req.body?.id ?? null,
      });
    }
  }
};

const postHandlers = config.authMode !== "none" && authMiddleware ? [authMiddleware, handleMcpPost] : [handleMcpPost];
app.post("/mcp", ...postHandlers);

const methodNotAllowed = (_req, res) => {
  res.status(405).json({
    jsonrpc: "2.0",
    error: {
      code: -32000,
      message: "Method not allowed.",
    },
    id: null,
  });
};

const getHandlers = config.authMode !== "none" && authMiddleware ? [authMiddleware, methodNotAllowed] : [methodNotAllowed];
const deleteHandlers = config.authMode !== "none" && authMiddleware ? [authMiddleware, methodNotAllowed] : [methodNotAllowed];
app.get("/mcp", ...getHandlers);
app.delete("/mcp", ...deleteHandlers);

app.listen(config.port, config.host, () => {
  console.log(`${runtimeServerName} listening on ${config.host}:${config.port}`);
  console.log(`Runtime mode: ${config.runtimeMode} (${config.platform.displayName})`);
  console.log(`Auth mode: ${config.authMode}`);
  if (config.publicBaseUrl) {
    console.log(`Public MCP URL: ${config.publicBaseUrl}/mcp`);
  }
});
