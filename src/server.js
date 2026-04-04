import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { createMcpExpressApp } from "@modelcontextprotocol/sdk/server/express.js";
import { mcpAuthRouter, getOAuthProtectedResourceMetadataUrl } from "@modelcontextprotocol/sdk/server/auth/router.js";
import { requireBearerAuth } from "@modelcontextprotocol/sdk/server/auth/middleware/bearerAuth.js";
import * as z from "zod/v4";

import { config, version } from "./config.js";
import {
  DockerCommandError,
  ensureDevboxRunning,
  execInDevbox,
  execReadOnlyInDevbox,
  getDevboxInfo,
  getDevboxGithubAuthStatus,
  getDevboxVersions,
  listFilesInDevbox,
  readFileInDevbox,
  readLargeFileInDevbox,
  recreateDevbox,
  restartDevbox,
  searchFilesInDevbox,
  syncGithubAuthToDevbox,
  stopDevbox,
  writeFileInDevbox,
  writeLargeFileInDevbox,
} from "./docker-runtime.js";
import {
  HostCommandError,
  getHostGithubAuthContext,
  getHostToolStatus,
  runAllowedProgram,
  runWindowsPowerShell,
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

const errorResult = (error, fallbackSummary = "The command failed.") => {
  if (error instanceof DockerCommandError || error instanceof HostCommandError) {
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
        exitCode: error.exitCode,
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
      name: "Docker ChatGPT Devbox MCP",
      version,
      websiteUrl: "https://docs.docker.com/",
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
        title: "Docker Devbox GitHub Auth Status",
        description: "Use this when you need to confirm whether the Docker devbox is authenticated to GitHub and which git identity is configured.",
        outputSchema,
      },
      "Checking Docker devbox GitHub auth",
      "Docker devbox GitHub auth checked",
    ),
    async () => {
      try {
        const data = await getDevboxGithubAuthStatus();
        return successResult("Fetched Docker devbox GitHub auth status.", { data });
      } catch (error) {
        return errorResult(error, "Failed to fetch Docker devbox GitHub auth status.");
      }
    },
  );

  server.registerTool(
    "devbox_sync_github_auth_from_host",
    safeActionTool(
      {
        title: "Sync Host GitHub Auth Into Docker Devbox",
        description:
          "Use this when the Windows host already has a valid GitHub CLI login and the Docker devbox should inherit GitHub authentication and git identity from the host.",
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

        return successResult("Synced the host GitHub CLI authentication into the Docker devbox.", {
          data: {
            ...data,
            hostUserName: hostGithub.userName || null,
            hostUserEmail: hostGithub.userEmail || null,
          },
        });
      } catch (error) {
        return errorResult(error, "Failed to sync host GitHub authentication into the Docker devbox.");
      }
    },
  );

  server.registerTool(
    "devbox_status",
    safeReadOnlyTool(
      {
        title: "Docker Devbox Status",
        description: "Use this when you need the current state of the reproducible Docker devbox and its installed toolchain.",
        outputSchema,
      },
      "Checking Docker devbox status",
      "Docker devbox status ready",
    ),
    async () => {
      try {
        const info = await getDevboxInfo();
        const data = {
          ...info,
          hostWorkspacePath: config.hostWorkspacePath,
          devboxWorkspacePath: config.devboxWorkspacePath,
          windowsHostExecEnabled: config.enableWindowsHostExec,
        };

        if (info.running) {
          data.versions = await getDevboxVersions();
        }

        return successResult("Fetched Docker devbox status.", { data });
      } catch (error) {
        return errorResult(error, "Failed to fetch Docker devbox status.");
      }
    },
  );

  server.registerTool(
    "devbox_start",
    safeActionTool(
      {
        title: "Start Docker Devbox",
        description: "Use this when the Docker devbox is stopped or missing and needs to be brought online.",
        outputSchema,
      },
      "Starting Docker devbox",
      "Docker devbox started",
    ),
    async () => {
      try {
        const info = await ensureDevboxRunning();
        return successResult(`Docker devbox ${info.name} is running.`, { data: info });
      } catch (error) {
        return errorResult(error, "Failed to start the Docker devbox.");
      }
    },
  );

  server.registerTool(
    "devbox_stop",
    safeActionTool(
      {
        title: "Stop Docker Devbox",
        description: "Use this when the Docker devbox should be shut down without deleting the workspace.",
        outputSchema,
      },
      "Stopping Docker devbox",
      "Docker devbox stopped",
    ),
    async () => {
      try {
        const info = await stopDevbox();
        return successResult(`Docker devbox ${info.name} is stopped.`, { data: info });
      } catch (error) {
        return errorResult(error, "Failed to stop the Docker devbox.");
      }
    },
  );

  server.registerTool(
    "devbox_restart",
    safeActionTool(
      {
        title: "Restart Docker Devbox",
        description: "Use this when the Docker devbox container needs a clean restart.",
        outputSchema,
      },
      "Restarting Docker devbox",
      "Docker devbox restarted",
    ),
    async () => {
      try {
        const info = await restartDevbox();
        return successResult(`Docker devbox ${info.name} has been restarted.`, { data: info });
      } catch (error) {
        return errorResult(error, "Failed to restart the Docker devbox.");
      }
    },
  );

  server.registerTool(
    "devbox_recreate",
    safeActionTool(
      {
        title: "Recreate Docker Devbox",
        description: "Use this when the Docker devbox container itself should be rebuilt from the configured image while preserving the mounted workspace.",
        outputSchema,
      },
      "Recreating Docker devbox",
      "Docker devbox recreated",
    ),
    async () => {
      try {
        const info = await recreateDevbox();
        return successResult(`Docker devbox ${info.name} has been recreated.`, { data: info });
      } catch (error) {
        return errorResult(error, "Failed to recreate the Docker devbox.");
      }
    },
  );

  server.registerTool(
    "devbox_exec_readonly",
    safeReadOnlyTool(
      {
        title: "Run Read-Only Shell Command In Docker Devbox",
        description:
          "Prefer this for inspection-only shell work such as ls, find, cat, sed -n, rg, git diff, git log, or config inspection. It runs in a disposable container with the workspace mounted read-only and network disabled, so project writes and outbound network access should fail.",
        inputSchema: {
          command: z.string().min(1).describe("Read-only shell command to run inside the Docker devbox."),
          working_dir: z.string().default(config.devboxWorkspacePath).describe("Working directory inside the Docker devbox."),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe("Linux user inside the disposable read-only container."),
        },
        outputSchema,
      },
      "Running read-only shell command in devbox",
      "Read-only devbox shell command finished",
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user }) => {
      try {
        const result = await execReadOnlyInDevbox({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          user,
        });
        return fromProcessResult(`Ran a read-only shell command in the Docker devbox at ${workingDir}.`, result);
      } catch (error) {
        return errorResult(error, "Failed to run the read-only Docker devbox shell command.");
      }
    },
  );

  server.registerTool(
    "devbox_exec",
    safeActionTool(
      {
        title: "Run Mutating Shell Command In Docker Devbox",
        description:
          "Use this only when the shell command needs side effects such as writing files, building artifacts, installing packages, changing git state, or otherwise mutating the devbox or workspace. Prefer devbox_exec_readonly for inspection, search, and file-reading commands.",
        inputSchema: {
          command: z.string().min(1).describe("Shell command to run inside the Docker devbox."),
          working_dir: z.string().default(config.devboxWorkspacePath).describe("Working directory inside the Docker devbox."),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
          user: z.string().default(config.devboxDefaultUser).describe("Linux user inside the devbox container."),
        },
        outputSchema,
      },
      "Running shell command in devbox",
      "Devbox shell command finished",
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds, user }) => {
      try {
        const result = await execInDevbox({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
          user,
        });
        return fromProcessResult(`Ran a shell command in the Docker devbox at ${workingDir}.`, result);
      } catch (error) {
        return errorResult(error, "Failed to run the Docker devbox shell command.");
      }
    },
  );

  server.registerTool(
    "devbox_list_files",
    safeReadOnlyTool(
      {
        title: "List Docker Devbox Files",
        description: "Use this when you need a directory listing inside the Docker devbox workspace or another container path.",
        inputSchema: {
          path: z.string().default(config.devboxWorkspacePath).describe("Directory path inside the Docker devbox."),
          recursive: z.boolean().default(false).describe("When true, recurse into subdirectories."),
          max_depth: z.number().int().min(1).max(20).default(4).describe("Maximum recursive depth when recursive is true."),
        },
        outputSchema,
      },
      "Listing Docker devbox files",
      "Docker devbox files listed",
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
        title: "Read Docker Devbox File",
        description: "Use this when you need text content from a file inside the Docker devbox.",
        inputSchema: {
          path: z.string().min(1).describe("File path inside the Docker devbox."),
          max_bytes: z.number().int().min(1).max(500000).default(65536).describe("Maximum bytes to return."),
        },
        outputSchema,
      },
      "Reading Docker devbox file",
      "Docker devbox file read",
    ),
    async ({ path, max_bytes: maxBytes }) => {
      try {
        const result = await readFileInDevbox({ path, maxBytes });
        return fromProcessResult(`Read ${path} from the Docker devbox.`, result);
      } catch (error) {
        return errorResult(error, `Failed to read ${path} from the Docker devbox.`);
      }
    },
  );

  server.registerTool(
    "devbox_read_large_file",
    safeReadOnlyTool(
      {
        title: "Read Large Docker Devbox File Chunk",
        description: "Use this when you need an exact byte range from a larger file inside the Docker devbox. It returns a base64 chunk plus metadata for safe paging without UTF-8 corruption.",
        inputSchema: {
          path: z.string().min(1).describe("File path inside the Docker devbox."),
          offset_bytes: z.number().int().min(0).default(0).describe("Starting byte offset within the file."),
          max_bytes: z.number().int().min(1).max(524288).default(262144).describe("Maximum raw bytes to return from that offset."),
        },
        outputSchema,
      },
      "Reading large Docker devbox file chunk",
      "Large Docker devbox file chunk read",
    ),
    async ({ path, offset_bytes: offsetBytes, max_bytes: maxBytes }) => {
      try {
        const data = await readLargeFileInDevbox({ path, offsetBytes, maxBytes });
        const summary = `Read ${path} from byte ${offsetBytes} in the Docker devbox.`;
        return successResult(summary, {
          data,
          text: textFromResult(summary, summarizeLargeReadData(data)),
        });
      } catch (error) {
        return errorResult(error, `Failed to read ${path} from byte ${offsetBytes} in the Docker devbox.`);
      }
    },
  );

  server.registerTool(
    "devbox_write_file",
    safeActionTool(
      {
        title: "Write Docker Devbox File",
        description: "Use this when you need to create or overwrite a text file inside the Docker devbox workspace.",
        inputSchema: {
          path: z.string().min(1).describe("File path inside the Docker devbox."),
          content: z.string().describe("UTF-8 file contents to write."),
          append: z.boolean().default(false).describe("Append to the file instead of overwriting it."),
          create_dirs: z.boolean().default(true).describe("Create parent directories if they do not exist."),
        },
        outputSchema,
      },
      "Writing Docker devbox file",
      "Docker devbox file written",
    ),
    async ({ path, content, append, create_dirs: createDirs }) => {
      try {
        const result = await writeFileInDevbox({ path, content, append, createDirs });
        const summary = append ? `Appended text to ${path} in the Docker devbox.` : `Wrote ${path} in the Docker devbox.`;
        return fromProcessResult(summary, result);
      } catch (error) {
        return errorResult(error, `Failed to write ${path} in the Docker devbox.`);
      }
    },
  );


  server.registerTool(
    "devbox_write_large_file",
    safeActionTool(
      {
        title: "Write Large Docker Devbox File",
        description: "Use this when you need to create or overwrite a large file inside the Docker devbox workspace using base64-backed transfer with post-write verification.",
        inputSchema: {
          path: z.string().min(1).describe("File path inside the Docker devbox."),
          content: z.string().optional().describe("Optional UTF-8 text payload to write. Prefer content_base64 for exact byte preservation."),
          content_base64: z.string().optional().describe("Base64-encoded raw bytes to write exactly as provided."),
          append: z.boolean().default(false).describe("Append to the file instead of overwriting it."),
          create_dirs: z.boolean().default(true).describe("Create parent directories if they do not exist."),
          expected_sha256: z.string().regex(/^[A-Fa-f0-9]{64}$/).optional().describe("Optional expected SHA-256 of the decoded payload for end-to-end verification."),
        },
        outputSchema,
      },
      "Writing large Docker devbox file",
      "Large Docker devbox file written",
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
          ? `Appended large payload to ${path} in the Docker devbox and verified the exact bytes.`
          : `Wrote large payload to ${path} in the Docker devbox and verified the exact bytes.`;
        return successResult(summary, {
          data,
          text: textFromResult(summary, summarizeLargeWriteData(data)),
        });
      } catch (error) {
        return errorResult(error, `Failed to write large payload to ${path} in the Docker devbox.`);
      }
    },
  );

  server.registerTool(
    "devbox_search_files",
    safeReadOnlyTool(
      {
        title: "Search Docker Devbox Files",
        description: "Use this when you need ripgrep-style text search inside the Docker devbox workspace.",
        inputSchema: {
          pattern: z.string().min(1).describe("Search pattern for ripgrep."),
          path: z.string().default(config.devboxWorkspacePath).describe("Directory path to search."),
          glob: z.string().default("*").describe("Optional glob filter."),
          case_sensitive: z.boolean().default(false).describe("When true, use case-sensitive search."),
          max_matches: z.number().int().min(1).max(5000).default(200).describe("Maximum number of matches to return."),
        },
        outputSchema,
      },
      "Searching Docker devbox files",
      "Docker devbox search finished",
    ),
    async ({ pattern, path, glob, case_sensitive: caseSensitive, max_matches: maxMatches }) => {
      try {
        const result = await searchFilesInDevbox({ pattern, path, glob, caseSensitive, maxMatches });
        return fromProcessResult(`Searched ${path} for "${pattern}" inside the Docker devbox.`, result);
      } catch (error) {
        return errorResult(error, `Failed to search ${path} inside the Docker devbox.`);
      }
    },
  );

  server.registerTool(
    "windows_host_status",
    safeReadOnlyTool(
      {
        title: "Windows Host Tool Status",
        description: "Use this when you need to inspect whether Windows host execution is enabled and which native programs are allowed.",
        outputSchema,
      },
      "Checking Windows host tool status",
      "Windows host tool status ready",
    ),
    async () => {
      try {
        return successResult("Fetched Windows host tool status.", {
          data: getHostToolStatus(),
        });
      } catch (error) {
        return errorResult(error, "Failed to fetch Windows host tool status.");
      }
    },
  );

  server.registerTool(
    "windows_host_exec",
    safeActionTool(
      {
        title: "Run Windows PowerShell Command",
        description:
          "Use this when you explicitly need native Windows host tooling rather than the reproducible Docker devbox, such as winget, host Git, host Docker CLI, or PowerShell automation.",
        inputSchema: {
          command: z.string().min(1).describe("PowerShell command to run on the Windows host."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe("Working directory on the Windows host."),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
        },
        outputSchema,
      },
      "Running Windows PowerShell command",
      "Windows PowerShell command finished",
    ),
    async ({ command, working_dir: workingDir, timeout_seconds: timeoutSeconds }) => {
      try {
        const result = await runWindowsPowerShell({
          command,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
        });
        return fromProcessResult(`Ran a Windows PowerShell command in ${workingDir}.`, result);
      } catch (error) {
        return errorResult(error, "Failed to run the Windows PowerShell command.");
      }
    },
  );

  server.registerTool(
    "windows_host_run_program",
    safeActionTool(
      {
        title: "Run Allowed Windows Host Program",
        description:
          "Use this when you need a specific allowed Windows host program such as git, docker, node, python, or winget with structured arguments.",
        inputSchema: {
          program: z.string().min(1).describe("Program name or path. It must be allowed by HOST_PROGRAM_ALLOWLIST."),
          args: z.array(z.string()).default([]).describe("Argument list for the program."),
          working_dir: z.string().default(config.hostDefaultWorkdir).describe("Working directory on the Windows host."),
          timeout_seconds: z.number().int().min(1).max(7200).default(300).describe("Command timeout in seconds."),
        },
        outputSchema,
      },
      "Running allowed Windows program",
      "Allowed Windows program finished",
    ),
    async ({ program, args, working_dir: workingDir, timeout_seconds: timeoutSeconds }) => {
      try {
        const result = await runAllowedProgram({
          program,
          args,
          workingDir,
          timeoutMs: (timeoutSeconds + 5) * 1000,
        });
        return fromProcessResult(`Ran ${program} on the Windows host.`, result);
      } catch (error) {
        return errorResult(error, `Failed to run ${program} on the Windows host.`);
      }
    },
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
      resourceName: "Docker ChatGPT Devbox MCP",
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
    name: "Docker ChatGPT Devbox MCP",
    version,
    auth_mode: config.authMode,
    public_base_url: config.publicBaseUrl || null,
    mcp_url: config.publicBaseUrl ? `${config.publicBaseUrl}/mcp` : null,
    oauth: oauthInfo,
    notes:
      config.authMode === "cloudflare-access"
        ? "Cloudflare Access-backed OAuth is enabled for ChatGPT app testing. Protect the /authorize path with a Cloudflare Access application."
        : config.authMode === "demo-oauth"
          ? "Demo OAuth is enabled for ChatGPT app testing. The Docker devbox is the main execution environment; Windows host tools are separate and explicit."
          : "No Authentication mode is active. The Docker devbox is the main execution environment; Windows host tools are separate and explicit.",
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
      devboxContainerName: config.devboxContainerName,
      devboxImageName: config.devboxImageName,
      devboxWorkspacePath: config.devboxWorkspacePath,
      hostWorkspacePath: config.hostWorkspacePath,
      windowsHostExecEnabled: config.enableWindowsHostExec,
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
  console.log(`Docker ChatGPT Devbox MCP listening on ${config.host}:${config.port}`);
  console.log(`Auth mode: ${config.authMode}`);
  if (config.publicBaseUrl) {
    console.log(`Public MCP URL: ${config.publicBaseUrl}/mcp`);
  }
});
