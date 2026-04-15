import { config } from "./config.js";
import { HostCommandError, getHostGithubAuthContext, runAllowedProgram } from "./host-tools.js";
import {
  getHostRuntimeInfo,
  ensureHostRuntimeReady,
  execInHostRuntime,
  execReadOnlyInHostRuntime,
  getHostRuntimeVersions,
  listFilesInHostRuntime,
  readFileInHostRuntime,
  readLargeFileInHostRuntime,
  searchFilesInHostRuntime,
  writeFileInHostRuntime,
  writeLargeFileInHostRuntime,
} from "./host-runtime.js";
import {
  DockerCommandError,
  ensureDevboxRunning as ensureDockerDevboxRunning,
  execInDevbox as execInDockerDevbox,
  execReadOnlyInDevbox as execReadOnlyInDockerDevbox,
  getDevboxGithubAuthStatus as getDockerDevboxGithubAuthStatus,
  getDevboxInfo as getDockerDevboxInfo,
  getDevboxVersions as getDockerDevboxVersions,
  listFilesInDevbox as listFilesInDockerDevbox,
  readFileInDevbox as readFileInDockerDevbox,
  readLargeFileInDevbox as readLargeFileInDockerDevbox,
  recreateDevbox as recreateDockerDevbox,
  restartDevbox as restartDockerDevbox,
  searchFilesInDevbox as searchFilesInDockerDevbox,
  syncGithubAuthToDevbox as syncGithubAuthToDockerDevbox,
  stopDevbox as stopDockerDevbox,
  writeFileInDevbox as writeFileInDockerDevbox,
  writeLargeFileInDevbox as writeLargeFileInDockerDevbox,
} from "./docker-runtime.js";

export const isDockerRuntime = config.runtimeMode === "docker";
export const runtimeTitle = isDockerRuntime ? "Docker Devbox" : `${config.platform.displayName} Host Devbox`;
export const runtimeLabel = isDockerRuntime ? "Docker devbox" : `${config.platform.displayName} host devbox`;
export const runtimeServerName = isDockerRuntime ? "Docker ChatGPT Devbox MCP" : `${config.platform.displayName} Host Devbox MCP`;
export const hostTitle = config.platform.isWindows ? "Windows Host" : `${config.platform.displayName} Host`;
export const hostCommandTitle = config.platform.isWindows ? "Windows PowerShell" : `${config.platform.displayName} Host Shell`;

const hostControlInfo = async (action) => ({
  ...(await getHostRuntimeInfo()),
  controlAction: action,
  controlMessage: `Host mode runs inside the current server process. Use the devbox launcher command to ${action} the service itself.`,
});

const getHostModeGithubAuthStatus = async () => {
  const context = await getHostGithubAuthContext();
  return {
    statusSummary: context.statusSummary,
    userName: context.userName,
    userEmail: context.userEmail,
  };
};

const syncGithubAuthToHostRuntime = async ({ token, userName, userEmail }) => {
  if (!token) {
    throw new HostCommandError("A GitHub token is required to sync auth into the host runtime.");
  }

  await runAllowedProgram({
    program: "gh",
    args: ["auth", "login", "--hostname", "github.com", "--with-token"],
    input: `${token}\n`,
    timeoutMs: 20000,
  });

  await runAllowedProgram({
    program: "gh",
    args: ["auth", "setup-git", "--hostname", "github.com"],
    timeoutMs: 15000,
  });

  if (userName) {
    await runAllowedProgram({
      program: "git",
      args: ["config", "--global", "user.name", userName],
      timeoutMs: 5000,
    });
  }

  if (userEmail) {
    await runAllowedProgram({
      program: "git",
      args: ["config", "--global", "user.email", userEmail],
      timeoutMs: 5000,
    });
  }

  return getHostModeGithubAuthStatus();
};

export const DockerOrHostCommandError = isDockerRuntime ? DockerCommandError : HostCommandError;

export const getDevboxInfo = () => (isDockerRuntime ? getDockerDevboxInfo() : getHostRuntimeInfo());
export const ensureDevboxRunning = () => (isDockerRuntime ? ensureDockerDevboxRunning() : ensureHostRuntimeReady());
export const stopDevbox = () => (isDockerRuntime ? stopDockerDevbox() : hostControlInfo("stop"));
export const restartDevbox = () => (isDockerRuntime ? restartDockerDevbox() : hostControlInfo("restart"));
export const recreateDevbox = () => (isDockerRuntime ? recreateDockerDevbox() : hostControlInfo("recreate"));
export const execInDevbox = (options) => (isDockerRuntime ? execInDockerDevbox(options) : execInHostRuntime(options));
export const execReadOnlyInDevbox = (options) => (isDockerRuntime ? execReadOnlyInDockerDevbox(options) : execReadOnlyInHostRuntime(options));
export const getDevboxVersions = () => (isDockerRuntime ? getDockerDevboxVersions() : getHostRuntimeVersions());
export const listFilesInDevbox = (options) => (isDockerRuntime ? listFilesInDockerDevbox(options) : listFilesInHostRuntime(options));
export const readFileInDevbox = (options) => (isDockerRuntime ? readFileInDockerDevbox(options) : readFileInHostRuntime(options));
export const readLargeFileInDevbox = (options) => (isDockerRuntime ? readLargeFileInDockerDevbox(options) : readLargeFileInHostRuntime(options));
export const writeFileInDevbox = (options) => (isDockerRuntime ? writeFileInDockerDevbox(options) : writeFileInHostRuntime(options));
export const writeLargeFileInDevbox = (options) => (isDockerRuntime ? writeLargeFileInDockerDevbox(options) : writeLargeFileInHostRuntime(options));
export const searchFilesInDevbox = (options) => (isDockerRuntime ? searchFilesInDockerDevbox(options) : searchFilesInHostRuntime(options));
export const getDevboxGithubAuthStatus = () =>
  (isDockerRuntime ? getDockerDevboxGithubAuthStatus() : getHostModeGithubAuthStatus());
export const syncGithubAuthToDevbox = (options) =>
  (isDockerRuntime ? syncGithubAuthToDockerDevbox(options) : syncGithubAuthToHostRuntime(options));
