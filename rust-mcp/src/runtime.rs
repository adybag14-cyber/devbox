#[cfg(windows)]
mod windows_host_shell;

use std::{
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant},
};

use futures::future::join_all;
#[cfg(windows)]
use tokio::sync::OnceCell;
use tokio::sync::{Mutex, mpsc::UnboundedSender};
use tokio_util::sync::CancellationToken;

use crate::{
    Config, RuntimeMode,
    process::{OutputChunk, ProcessError, ProcessOptions, ProcessOutput, spawn_process},
};

#[derive(Debug, Clone)]
pub struct ProgramRequest {
    pub program: String,
    pub args: Vec<String>,
    pub input: Option<Vec<u8>>,
    pub working_dir: PathBuf,
    pub timeout: Duration,
    pub user: String,
    pub max_capture_chars: Option<usize>,
    pub output_tx: Option<UnboundedSender<OutputChunk>>,
    pub pid_tx: Option<UnboundedSender<u32>>,
}

#[derive(Debug, Clone)]
pub struct ShellRequest {
    pub command: String,
    pub working_dir: PathBuf,
    pub timeout: Duration,
    pub user: String,
    pub max_capture_chars: Option<usize>,
    pub output_tx: Option<UnboundedSender<OutputChunk>>,
    pub pid_tx: Option<UnboundedSender<u32>>,
}

#[derive(Debug, Clone)]
pub enum RuntimeExecError {
    HostExecDisabled,
    ProgramNotAllowed {
        program: String,
        allowlist: Vec<String>,
    },
    HostProgramNotAllowed {
        program: String,
        allowlist: Vec<String>,
    },
    WindowsElevationRequired,
    WindowsAdminProbe(String),
    Process(ProcessError),
}

impl std::fmt::Display for RuntimeExecError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::HostExecDisabled => formatter.write_str("Windows host execution is disabled."),
            Self::ProgramNotAllowed { program, allowlist } => write!(
                formatter,
                "Program \"{program}\" is not in DEVBOX_PROGRAM_ALLOWLIST: {}",
                allowlist.join(", ")
            ),
            Self::HostProgramNotAllowed { program, allowlist } => write!(
                formatter,
                "Program \"{program}\" is not in HOST_PROGRAM_ALLOWLIST: {}",
                allowlist.join(", ")
            ),
            Self::WindowsElevationRequired => formatter.write_str(
                "Windows host PowerShell requires the Devbox MCP process to already be elevated. This MCP process is medium-integrity, so host_exec refused to call Start-Process -Verb RunAs (that would spam UAC). Guardian treats unelevated MCP as unhealthy and restarts it via the Highest scheduled-task path. Retry after repair.",
            ),
            Self::WindowsAdminProbe(message) => formatter.write_str(message),
            Self::Process(error) => std::fmt::Display::fmt(error, formatter),
        }
    }
}

impl std::error::Error for RuntimeExecError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Process(error) => Some(error),
            _ => None,
        }
    }
}

impl From<ProcessError> for RuntimeExecError {
    fn from(value: ProcessError) -> Self {
        Self::Process(value)
    }
}

#[derive(Debug, Clone)]
struct VersionCacheEntry {
    value: Vec<String>,
    expires_at: Instant,
}

#[derive(Debug, Clone)]
pub struct RuntimeExecutor {
    config: Arc<Config>,
    versions_cache: Arc<Mutex<Option<VersionCacheEntry>>>,
    #[cfg(windows)]
    windows_admin_state: Arc<OnceCell<bool>>,
}

impl RuntimeExecutor {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        Self {
            config,
            versions_cache: Arc::new(Mutex::new(None)),
            #[cfg(windows)]
            windows_admin_state: Arc::new(OnceCell::new()),
        }
    }

    #[must_use]
    pub fn config(&self) -> &Config {
        &self.config
    }

    /// Warm and return the cached Windows administrator state used by host shell execution.
    ///
    /// # Errors
    /// Returns `PowerShell` launch or parse failures while probing the current process identity.
    #[cfg(windows)]
    pub async fn warm_host_execution_state(
        &self,
        cancellation: CancellationToken,
    ) -> Result<Option<bool>, RuntimeExecError> {
        if !self.config.host_exec_enabled || !self.config.platform.is_windows {
            return Ok(None);
        }
        self.windows_admin_state(cancellation).await.map(Some)
    }

    /// Non-Windows hosts have no Windows administrator state to warm.
    #[cfg(not(windows))]
    pub fn warm_host_execution_state(
        &self,
        _cancellation: CancellationToken,
    ) -> std::future::Ready<Result<Option<bool>, RuntimeExecError>> {
        std::future::ready(Ok(None))
    }

    /// Read cached toolchain versions from the selected runtime, refreshing at the JS-configured TTL.
    ///
    /// # Errors
    /// Docker mode returns an error if the version probe command cannot run. Host mode mirrors the
    /// JavaScript implementation by reporting individual unavailable programs without failing status.
    pub async fn get_versions(
        &self,
        force: bool,
        cancellation: CancellationToken,
    ) -> Result<Vec<String>, RuntimeExecError> {
        {
            let cache = self.versions_cache.lock().await;
            if !force
                && let Some(entry) = cache.as_ref()
                && Instant::now() < entry.expires_at
            {
                return Ok(entry.value.clone());
            }
        }
        // External version probes must never run while the cache mutex is held. A
        // duplicate stale refresh is cheaper than serializing unrelated status calls
        // behind a slow executable or Docker probe.
        let value = match self.config.runtime_mode {
            RuntimeMode::Host => self.load_host_versions(cancellation).await,
            RuntimeMode::Docker => self.load_docker_versions(cancellation).await?,
        };
        let mut cache = self.versions_cache.lock().await;
        *cache = Some(VersionCacheEntry {
            value: value.clone(),
            expires_at: Instant::now() + Duration::from_millis(self.config.devbox_version_cache_ms),
        });
        Ok(value)
    }

    /// Return the most recently warmed version snapshot without executing external programs.
    pub async fn cached_versions(&self) -> Option<Vec<String>> {
        let cache = self.versions_cache.lock().await;
        cache
            .as_ref()
            .filter(|entry| Instant::now() < entry.expires_at)
            .map(|entry| entry.value.clone())
    }

    async fn load_host_versions(&self, cancellation: CancellationToken) -> Vec<String> {
        let candidates: Vec<(&str, &[&str])> = if self.config.platform.is_windows {
            vec![
                ("node", &["--version"]),
                ("npm", &["--version"]),
                ("git", &["--version"]),
                ("gh", &["--version"]),
                ("python", &["--version"]),
            ]
        } else {
            vec![
                ("node", &["--version"]),
                ("npm", &["--version"]),
                ("git", &["--version"]),
                ("gh", &["--version"]),
                ("python3", &["--version"]),
                ("rg", &["--version"]),
            ]
        };
        let probes = candidates.into_iter().map(|(program, args)| {
            let arguments = args
                .iter()
                .map(|value| (*value).to_owned())
                .collect::<Vec<_>>();
            #[cfg(windows)]
            let (executable, arguments) =
                resolve_windows_host_program(&self.config, program, &arguments);
            #[cfg(not(windows))]
            let executable = if program == "node" {
                self.config.node_exe.clone()
            } else {
                program.to_owned()
            };
            let cwd = self.config.host_default_workdir.clone();
            let cancellation = cancellation.child_token();
            async move {
                match spawn_process(
                    &executable,
                    &arguments,
                    ProcessOptions {
                        cwd: Some(cwd),
                        timeout: Some(Duration::from_secs(15)),
                        max_capture_chars: Some(16_384),
                        ..ProcessOptions::default()
                    },
                    cancellation,
                )
                .await
                {
                    Ok(output) => {
                        let combined = format!("{}{}", output.stdout, output.stderr);
                        let value = combined
                            .lines()
                            .find(|line| !line.trim().is_empty())
                            .unwrap_or("available");
                        format!("{program}={}", value.trim())
                    }
                    Err(_) => format!("{program}=unavailable"),
                }
            }
        });
        join_all(probes).await
    }

    async fn load_docker_versions(
        &self,
        cancellation: CancellationToken,
    ) -> Result<Vec<String>, RuntimeExecError> {
        let command = [
            "printf 'gh='; if command -v gh >/dev/null 2>&1; then printf 'installed\\n'; else printf 'missing\\n'; fi",
            "printf 'node='; node --version",
            "printf 'npm='; if command -v npm >/dev/null 2>&1; then printf 'installed\\n'; else printf 'missing\\n'; fi",
            "printf 'python='; python3 --version",
            "printf 'git='; git --version",
            "printf 'rg='; rg --version | head -n 1",
        ]
        .join(" && ");
        let output = spawn_process(
            "docker",
            &[
                "exec".to_owned(),
                "-w".to_owned(),
                self.config
                    .devbox_workspace_path
                    .to_string_lossy()
                    .into_owned(),
                self.config.devbox_container_name.clone(),
                "bash".to_owned(),
                "-lc".to_owned(),
                command,
            ],
            ProcessOptions {
                timeout: Some(Duration::from_secs(20)),
                max_capture_chars: Some(32_768),
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await?;
        Ok(output
            .stdout
            .lines()
            .map(str::trim)
            .filter(|line| !line.is_empty())
            .map(str::to_owned)
            .collect())
    }

    /// Run one allowlisted executable directly in the selected Devbox runtime.
    ///
    /// # Errors
    /// Returns an error when host execution is disabled, the normalized executable is
    /// outside the configured allowlist, or the child process fails/times out/is cancelled.
    pub async fn run_program(
        &self,
        mut request: ProgramRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let normalized = normalize_program(&request.program);
        if normalized.is_empty() || !self.config.devbox_program_allowlist.contains(&normalized) {
            return Err(RuntimeExecError::ProgramNotAllowed {
                program: request.program,
                allowlist: self.config.devbox_program_allowlist.clone(),
            });
        }
        request.program = normalized;
        match self.config.runtime_mode {
            RuntimeMode::Host => self.run_host_program(request, cancellation).await,
            RuntimeMode::Docker => self.run_docker_program(request, cancellation).await,
        }
    }

    /// Run one allowlisted executable directly on the host regardless of the selected Devbox runtime.
    ///
    /// # Errors
    /// Returns an error when host execution is disabled, the program is outside
    /// `HOST_PROGRAM_ALLOWLIST`, or the child process fails.
    pub async fn run_host_program_only(
        &self,
        mut request: ProgramRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let normalized = normalize_program(&request.program);
        if normalized.is_empty() || !self.config.host_program_allowlist.contains(&normalized) {
            return Err(RuntimeExecError::HostProgramNotAllowed {
                program: request.program,
                allowlist: self.config.host_program_allowlist.clone(),
            });
        }
        request.program = normalized;
        self.run_host_program(request, cancellation).await
    }

    async fn run_host_program(
        &self,
        request: ProgramRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        if !self.config.host_exec_enabled {
            return Err(RuntimeExecError::HostExecDisabled);
        }
        #[cfg(windows)]
        let (executable, arguments) =
            resolve_windows_host_program(&self.config, &request.program, &request.args);
        #[cfg(not(windows))]
        let (executable, arguments) = if request.program == "node" {
            (self.config.node_exe.clone(), request.args.clone())
        } else {
            (request.program.clone(), request.args.clone())
        };
        spawn_process(
            &executable,
            &arguments,
            ProcessOptions {
                cwd: Some(request.working_dir),
                timeout: Some(request.timeout),
                max_capture_chars: request.max_capture_chars,
                input: request.input,
                output_tx: request.output_tx,
                pid_tx: request.pid_tx,
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await
        .map_err(Into::into)
    }

    async fn run_docker_program(
        &self,
        request: ProgramRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let mut docker_args = vec!["exec".to_owned()];
        if request.input.is_some() {
            docker_args.push("-i".to_owned());
        }
        if !request.user.trim().is_empty() {
            docker_args.push("-u".to_owned());
            docker_args.push(request.user);
        }
        docker_args.push("-w".to_owned());
        docker_args.push(request.working_dir.to_string_lossy().into_owned());
        docker_args.push(self.config.devbox_container_name.clone());
        docker_args.push(request.program);
        docker_args.extend(request.args);
        spawn_process(
            "docker",
            &docker_args,
            ProcessOptions {
                timeout: Some(request.timeout),
                max_capture_chars: request.max_capture_chars,
                input: request.input,
                output_tx: request.output_tx,
                pid_tx: request.pid_tx,
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await
        .map_err(Into::into)
    }

    /// Run one shell command using the same host/Docker shell shape as the JavaScript MCP.
    ///
    /// # Errors
    /// Returns an error when host execution is disabled or the shell process fails.
    pub async fn run_shell(
        &self,
        request: ShellRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        match self.config.runtime_mode {
            RuntimeMode::Host => self.run_host_shell(request, cancellation).await,
            RuntimeMode::Docker => self.run_docker_shell(request, cancellation).await,
        }
    }

    /// Run one shell command directly on the host regardless of the selected Devbox runtime.
    ///
    /// # Errors
    /// Returns an error when host execution is disabled or the shell process fails.
    pub async fn run_host_shell_only(
        &self,
        request: ShellRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        self.run_host_shell(request, cancellation).await
    }

    async fn run_host_shell(
        &self,
        request: ShellRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        if !self.config.host_exec_enabled {
            return Err(RuntimeExecError::HostExecDisabled);
        }
        #[cfg(windows)]
        {
            return self.run_windows_host_shell(request, cancellation).await;
        }
        #[cfg(not(windows))]
        {
            let args = host_shell_args(
                &self.config.host_shell,
                &request.command,
                self.config.platform.is_windows,
            );
            spawn_process(
                &self.config.host_shell,
                &args,
                ProcessOptions {
                    cwd: Some(request.working_dir),
                    timeout: Some(request.timeout),
                    max_capture_chars: request.max_capture_chars,
                    output_tx: request.output_tx,
                    pid_tx: request.pid_tx,
                    ..ProcessOptions::default()
                },
                cancellation,
            )
            .await
            .map_err(Into::into)
        }
    }

    async fn run_docker_shell(
        &self,
        request: ShellRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let mut args = vec!["exec".to_owned()];
        if !request.user.trim().is_empty() {
            args.push("-u".to_owned());
            args.push(request.user);
        }
        args.extend([
            "-w".to_owned(),
            request.working_dir.to_string_lossy().into_owned(),
            self.config.devbox_container_name.clone(),
            "bash".to_owned(),
            "-lc".to_owned(),
            request.command,
        ]);
        spawn_process(
            "docker",
            &args,
            ProcessOptions {
                timeout: Some(request.timeout),
                max_capture_chars: request.max_capture_chars,
                output_tx: request.output_tx,
                pid_tx: request.pid_tx,
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await
        .map_err(Into::into)
    }
}

#[must_use]
pub fn normalize_program(program: &str) -> String {
    let basename = program.rsplit(['\\', '/']).next().unwrap_or_default();
    let normalized = basename.to_ascii_lowercase();
    for suffix in [".exe", ".com", ".cmd", ".bat", ".ps1"] {
        if let Some(value) = normalized.strip_suffix(suffix) {
            return value.to_owned();
        }
    }
    normalized
}

#[cfg(windows)]
fn resolve_windows_host_program(
    config: &Config,
    program: &str,
    args: &[String],
) -> (String, Vec<String>) {
    if program.eq_ignore_ascii_case("node") {
        return (config.node_exe.clone(), args.to_vec());
    }
    let path = resolve_windows_program_path(program).unwrap_or_else(|| PathBuf::from(program));
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or_default()
        .to_ascii_lowercase();
    if extension == "ps1" {
        let mut wrapped = vec![
            "-NoLogo".to_owned(),
            "-NoProfile".to_owned(),
            "-NonInteractive".to_owned(),
            "-ExecutionPolicy".to_owned(),
            "Bypass".to_owned(),
            "-File".to_owned(),
            path.to_string_lossy().into_owned(),
        ];
        wrapped.extend(args.iter().cloned());
        return (config.power_shell_exe.clone(), wrapped);
    }
    (path.to_string_lossy().into_owned(), args.to_vec())
}

#[cfg(windows)]
fn resolve_windows_program_path(program: &str) -> Option<PathBuf> {
    let supplied = PathBuf::from(program);
    if supplied.components().count() > 1 && supplied.is_file() {
        return Some(supplied);
    }
    let path = std::env::var_os("PATH")?;
    let has_extension = supplied.extension().is_some();
    let extensions = std::env::var("PATHEXT")
        .unwrap_or_else(|_| ".COM;.EXE;.BAT;.CMD".to_owned())
        .split(';')
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
        .collect::<Vec<_>>();
    for directory in std::env::split_paths(&path) {
        if has_extension {
            let candidate = directory.join(program);
            if candidate.is_file() {
                return Some(candidate);
            }
            continue;
        }
        for extension in &extensions {
            let candidate = directory.join(format!("{program}{extension}"));
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    None
}

#[cfg(any(not(windows), test))]
fn host_shell_args(shell: &str, command: &str, is_windows: bool) -> Vec<String> {
    let name = normalize_program(shell);
    if is_windows && matches!(name.as_str(), "powershell" | "pwsh") {
        return [
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            command,
        ]
        .into_iter()
        .map(str::to_owned)
        .collect();
    }
    if is_windows && name == "cmd" {
        return ["/d", "/s", "/c", command]
            .into_iter()
            .map(str::to_owned)
            .collect();
    }
    vec!["-lc".to_owned(), command.to_owned()]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn program_normalization_matches_javascript_behavior() {
        assert_eq!(normalize_program(r"C:\\Tools\\Git.EXE"), "git");
        assert_eq!(normalize_program("/usr/bin/python3"), "python3");
        assert_eq!(normalize_program("node"), "node");
    }

    fn test_config(root: &std::path::Path) -> Arc<Config> {
        Arc::new(Config {
            project_root: root.to_path_buf(),
            host: "127.0.0.1".to_owned(),
            port: 0,
            auth_mode: crate::AuthMode::None,
            runtime_mode: RuntimeMode::Host,
            platform: crate::Platform::detect(),
            public_base_url: None,
            gateway_bridge: crate::config::GatewayBridgeConfig {
                enabled: false,
                origins: vec![
                    "https://chatgpt.com".to_owned(),
                    "https://chat.openai.com".to_owned(),
                ],
            },
            oauth_state_file_path: root.join("oauth-state.json"),
            cloudflare_access_team_domain: None,
            cloudflare_access_aud: String::new(),
            cloudflare_access_jwks_url: None,
            host_workspace_path: root.to_path_buf(),
            devbox_workspace_path: root.to_path_buf(),
            devbox_container_name: "unused".to_owned(),
            devbox_image_name: "chatgpt-devbox-runtime:local".to_owned(),
            devbox_tmp_volume_name: "chatgpt-devbox-runtime-tmp".to_owned(),
            devbox_retired_container_grace_ms: 300_000,
            devbox_auto_start: true,
            devbox_version_cache_ms: 120_000,
            docker_command_timeout_ms: 120_000,
            devbox_default_user: String::new(),
            host_default_workdir: root.to_path_buf(),
            host_shell: if cfg!(windows) { "cmd.exe" } else { "/bin/sh" }.to_owned(),
            power_shell_exe: if cfg!(windows) { "pwsh.exe" } else { "" }.to_owned(),
            power_shell_fallback_exe: if cfg!(windows) { "powershell.exe" } else { "" }.to_owned(),
            node_exe: "node".to_owned(),
            host_program_allowlist: vec!["rustc".to_owned()],
            host_search_backend: crate::config::HostSearchBackend::Auto,
            devbox_program_allowlist: vec!["rustc".to_owned()],
            host_exec_enabled: true,
            allow_windows_host_exec_uac: false,
            execution_slot_root: root.join("execution-slots"),
            jobs_root: root.join("jobs"),
            mcp_performance_state_path: root.join("mcp-performance.json"),
            usage_log: crate::config::UsageLogConfig {
                max_bytes: 16 * 1024 * 1024,
                rotations: 3,
            },
            mcp_json_body_limit_bytes: 16 * 1024 * 1024,
            exec_max_concurrent: 6,
            exec_reserved_interactive: 1,
            exec_queue_timeout_ms: 15_000,
            background_queue_timeout_ms: 300_000,
            watch_max_concurrent: 4,
            exec_heavy_weight: 2,
            job_log_max_bytes: 32 * 1024 * 1024,
            job_log_rotations: 2,
            job_heartbeat_ms: 5_000,
            job_orphan_stale_ms: 15_000,
            job_retention_hours: 168,
            screen_capture_attempt_timeout_ms: 8_000,
            screen_capture_retries: 1,
            screen_capture_queue_timeout_ms: 5_000,
            max_wait_seconds: 300.0,
            command_output_limit_chars: 65_536,
            max_mcp_transfer_chars: 4_000_000,
        })
    }

    #[tokio::test]
    async fn host_only_program_ignores_selected_docker_runtime() {
        let temp = tempfile::tempdir().unwrap();
        let mut config = (*test_config(temp.path())).clone();
        config.runtime_mode = RuntimeMode::Docker;
        let runtime = RuntimeExecutor::new(Arc::new(config));
        let output = runtime
            .run_host_program_only(
                ProgramRequest {
                    program: "rustc".to_owned(),
                    args: vec!["--version".to_owned()],
                    input: None,
                    working_dir: temp.path().to_path_buf(),
                    timeout: Duration::from_secs(10),
                    user: String::new(),
                    max_capture_chars: Some(4_096),
                    output_tx: None,
                    pid_tx: None,
                },
                CancellationToken::new(),
            )
            .await
            .expect("host-only direct program");
        assert!(output.stdout.starts_with("rustc "));
    }

    #[tokio::test]
    async fn host_direct_program_runs_without_shell_parsing() {
        let temp = tempfile::tempdir().unwrap();
        let executor = RuntimeExecutor::new(test_config(temp.path()));
        let output = executor
            .run_program(
                ProgramRequest {
                    program: "rustc".to_owned(),
                    args: vec!["--version".to_owned()],
                    input: None,
                    working_dir: temp.path().to_path_buf(),
                    timeout: Duration::from_secs(15),
                    user: String::new(),
                    max_capture_chars: Some(4096),
                    output_tx: None,
                    pid_tx: None,
                },
                CancellationToken::new(),
            )
            .await
            .unwrap();
        assert_eq!(output.exit_code, 0);
        assert!(output.stdout.starts_with("rustc "));
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn windows_version_probe_resolves_npm_command_shim() {
        let temp = tempfile::tempdir().unwrap();
        let executor = RuntimeExecutor::new(test_config(temp.path()));
        let versions = executor
            .get_versions(true, CancellationToken::new())
            .await
            .expect("Windows host version probes should complete");
        let npm = versions
            .iter()
            .find(|line| line.starts_with("npm="))
            .expect("npm version entry should be present");
        assert_ne!(npm, "npm=unavailable");
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn windows_direct_program_resolves_npm_command_shim() {
        let temp = tempfile::tempdir().unwrap();
        let mut config = (*test_config(temp.path())).clone();
        config.host_program_allowlist.push("npm".to_owned());
        config.devbox_program_allowlist.push("npm".to_owned());
        let executor = RuntimeExecutor::new(Arc::new(config));
        let resolved =
            resolve_windows_program_path("npm").expect("npm shim should resolve from PATH");
        assert!(matches!(
            resolved
                .extension()
                .and_then(|value| value.to_str())
                .map(str::to_ascii_lowercase)
                .as_deref(),
            Some("cmd" | "exe" | "com" | "ps1")
        ));
        let output = executor
            .run_program(
                ProgramRequest {
                    program: "npm".to_owned(),
                    args: vec!["--version".to_owned()],
                    input: None,
                    working_dir: temp.path().to_path_buf(),
                    timeout: Duration::from_secs(15),
                    user: String::new(),
                    max_capture_chars: Some(4096),
                    output_tx: None,
                    pid_tx: None,
                },
                CancellationToken::new(),
            )
            .await
            .expect("npm direct shim execution");
        assert_eq!(output.exit_code, 0);
        assert!(!output.stdout.trim().is_empty());
    }

    #[tokio::test]
    async fn direct_program_rejects_normalized_name_outside_allowlist() {
        let temp = tempfile::tempdir().unwrap();
        let executor = RuntimeExecutor::new(test_config(temp.path()));
        let error = executor
            .run_program(
                ProgramRequest {
                    program: if cfg!(windows) {
                        r"C:\Windows\System32\cmd.exe".to_owned()
                    } else {
                        "/bin/sh".to_owned()
                    },
                    args: Vec::new(),
                    input: None,
                    working_dir: temp.path().to_path_buf(),
                    timeout: Duration::from_secs(1),
                    user: String::new(),
                    max_capture_chars: Some(1024),
                    output_tx: None,
                    pid_tx: None,
                },
                CancellationToken::new(),
            )
            .await
            .unwrap_err();
        assert!(matches!(error, RuntimeExecError::ProgramNotAllowed { .. }));
    }

    #[test]
    fn shell_arguments_match_windows_powershell_and_cmd_contract() {
        assert_eq!(
            host_shell_args("pwsh.exe", "Get-Location", true),
            vec![
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                "Get-Location"
            ]
        );
        assert_eq!(
            host_shell_args("cmd.exe", "echo hi", true),
            vec!["/d", "/s", "/c", "echo hi"]
        );
        assert_eq!(
            host_shell_args("/bin/bash", "pwd", false),
            vec!["-lc", "pwd"]
        );
    }
}
