use std::{
    net::SocketAddr,
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant, UNIX_EPOCH},
};

use anyhow::{Context, Result};
use axum::{
    Json, Router,
    body::Body,
    extract::{Extension, State},
    http::{HeaderMap, StatusCode, header},
    middleware,
    response::{IntoResponse, Response},
    routing::get,
};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use rmcp::{
    RoleServer, ServerHandler,
    handler::server::wrapper::Parameters,
    handler::server::{router::tool::ToolRouter, tool::ToolCallContext},
    model::{
        CallToolRequestParams, CallToolResponse, CallToolResult, Implementation,
        ServerCapabilities, ServerInfo,
    },
    schemars,
    service::RequestContext,
    tool, tool_handler, tool_router,
    transport::streamable_http_server::{
        StreamableHttpServerConfig, StreamableHttpService, session::local::LocalSessionManager,
    },
};
use serde::Deserialize;
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;
use tower_http::trace::TraceLayer;

use crate::{
    AuthMode, Config, RuntimeMode,
    capture::CaptureService,
    docker_files::{DockerFileBackend, DockerListOptions},
    execution::{AcquireRequest, ExecutionScheduler, SchedulerConfig},
    files::{FileService, LargeReadResult, LargeWriteResult, ListOptions, ProcessResult},
    gateway::{GatewayRequestContext, GatewayState, transport_allowed_hosts},
    github_auth::GithubAuthService,
    host_inspect::{InspectFileRequest, inspect_host_file},
    job_manager::{JobManager, StartProgramJob},
    lifecycle::{LifecycleAction, LifecycleService},
    oauth::OAuthService,
    output::{OutputMode, shape_process_output},
    performance::PerformanceMonitor,
    request_control::ActiveRequestRegistry,
    result::ToolEnvelope,
    runtime::{ProgramRequest, RuntimeExecError, RuntimeExecutor, ShellRequest},
    search::{SearchRequest, SearchService},
    usage::{ToolUsageDropGuard, ToolUsageInvocation, UsageService},
};

#[derive(Debug, Clone)]
pub struct DevboxMcp {
    config: Arc<Config>,
    files: Arc<FileService>,
    docker_files: Arc<DockerFileBackend>,
    scheduler: Arc<ExecutionScheduler>,
    runtime: Arc<RuntimeExecutor>,
    jobs: Arc<JobManager>,
    search: Arc<SearchService>,
    lifecycle: Arc<LifecycleService>,
    github_auth: Arc<GithubAuthService>,
    capture: Arc<CaptureService>,
    performance: Arc<PerformanceMonitor>,
    usage: Arc<UsageService>,
    active_requests: Arc<ActiveRequestRegistry>,
    tool_router: ToolRouter<Self>,
}

impl DevboxMcp {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        let scheduler = Arc::new(ExecutionScheduler::new(SchedulerConfig {
            root: config.execution_slot_root.clone(),
            max_concurrent: config.exec_max_concurrent,
            reserved_interactive: config.exec_reserved_interactive,
            watch_max_concurrent: config.watch_max_concurrent,
            queue_timeout: Duration::from_millis(config.exec_queue_timeout_ms),
            heavy_weight: config.exec_heavy_weight,
        }));
        let runtime = Arc::new(RuntimeExecutor::new(config.clone()));
        let lifecycle = Arc::new(LifecycleService::new(config.clone()));
        let github_auth = Arc::new(GithubAuthService::new(
            config.clone(),
            runtime.clone(),
            lifecycle.clone(),
        ));
        let capture = Arc::new(CaptureService::new(config.clone()));
        let performance = Arc::new(PerformanceMonitor::new(
            config.mcp_performance_state_path.clone(),
        ));
        let usage = Arc::new(UsageService::new(
            &config.project_root,
            config.usage_log.max_bytes,
            config.usage_log.rotations,
        ));
        let active_requests = Arc::new(ActiveRequestRegistry::new());
        Self {
            runtime,
            jobs: Arc::new(JobManager::new(config.clone())),
            search: Arc::new(SearchService::new(config.clone())),
            lifecycle,
            github_auth,
            capture,
            performance,
            usage,
            active_requests,
            config,
            files: Arc::new(FileService::new()),
            docker_files: Arc::new(DockerFileBackend::new()),
            scheduler,
            tool_router: Self::tool_router(),
        }
    }

    #[must_use]
    pub fn config(&self) -> &Config {
        &self.config
    }

    async fn capture_display_internal(
        &self,
        request: CaptureDisplayRequest,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        match self.capture.display(request.quality, cancellation).await {
            Ok(capture) => ToolEnvelope::image_success(
                format!(
                    "Captured the {} display.",
                    host_title(&self.config).to_lowercase()
                ),
                Some(capture.metadata),
                STANDARD.encode(capture.image),
                capture.mime_type,
            ),
            Err(error) => render_anyhow_tool_error(&error, self.config.command_output_limit_chars),
        }
    }

    async fn capture_window_internal(
        &self,
        request: CaptureWindowRequest,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.pid == 0 {
            return ToolEnvelope::error("pid must be a positive process ID.", None);
        }
        if self.config.platform.is_windows && request.pid > i32::MAX as u64 {
            return render_windows_capture_pid_binding_error(request.pid);
        }
        let Ok(pid) = u32::try_from(request.pid) else {
            return render_oversized_posix_capture_pid(
                &self.config,
                request.pid,
                self.config.command_output_limit_chars,
            );
        };
        match self
            .capture
            .program(
                pid,
                request.quality,
                request.include_process_tree,
                cancellation,
            )
            .await
        {
            Ok(capture) => ToolEnvelope::image_success(
                format!(
                    "Captured {} window for PID {}.",
                    host_title(&self.config),
                    request.pid
                ),
                Some(capture.metadata),
                STANDARD.encode(capture.image),
                capture.mime_type,
            ),
            Err(error) => render_anyhow_tool_error(&error, self.config.command_output_limit_chars),
        }
    }
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct WaitRequest {
    #[schemars(description = "How long to wait, in seconds.")]
    seconds: f64,
    #[serde(default)]
    #[schemars(description = "Optional human-readable reason for telemetry/context.")]
    reason: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct WaitForFileRequest {
    #[schemars(description = "Host filesystem path to watch.")]
    path: String,
    #[serde(default = "default_true")]
    #[schemars(description = "Wait for the path to exist; false waits for removal.")]
    should_exist: bool,
    #[serde(default)]
    #[schemars(description = "When waiting for existence, require at least this file size.")]
    min_bytes: u64,
    #[serde(default)]
    #[schemars(description = "Require the condition to remain true for this duration.")]
    stable_ms: u64,
    #[serde(default)]
    #[schemars(description = "Maximum wait duration in seconds.")]
    timeout_seconds: Option<f64>,
    #[serde(default = "default_poll_ms")]
    #[schemars(description = "Filesystem poll interval in milliseconds.")]
    poll_ms: u64,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct DevboxListFilesRequest {
    #[serde(default)]
    path: String,
    #[serde(default)]
    recursive: bool,
    #[serde(default = "default_list_depth")]
    max_depth: usize,
    #[serde(default = "default_list_entries")]
    max_entries: usize,
    #[serde(default = "default_list_timeout")]
    timeout_seconds: u64,
    #[serde(default = "default_excluded_directories")]
    exclude_directories: Vec<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct DevboxReadFileRequest {
    path: String,
    #[serde(default = "default_read_bytes")]
    max_bytes: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct DevboxLargeReadRequest {
    path: String,
    #[serde(default)]
    offset_bytes: u64,
    #[serde(default = "default_large_read_bytes")]
    max_bytes: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct DevboxWriteFileRequest {
    path: String,
    content: String,
    #[serde(default)]
    append: bool,
    #[serde(default = "default_true")]
    create_dirs: bool,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct DevboxLargeWriteRequest {
    path: String,
    #[serde(default)]
    content: Option<String>,
    #[serde(default)]
    content_base64: Option<String>,
    #[serde(default)]
    append: bool,
    #[serde(default = "default_true")]
    create_dirs: bool,
    #[serde(default)]
    expected_sha256: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct SearchFilesRequest {
    pattern: String,
    #[serde(default)]
    path: String,
    #[serde(default = "default_search_glob")]
    glob: String,
    #[serde(default)]
    case_sensitive: bool,
    #[serde(default = "default_search_matches")]
    max_matches: usize,
    #[serde(default = "default_search_depth")]
    max_depth: usize,
    #[serde(default = "default_search_file_bytes")]
    max_file_bytes: u64,
    #[serde(default = "default_search_timeout")]
    timeout_seconds: u64,
    #[serde(default = "default_excluded_directories")]
    exclude_directories: Vec<String>,
    #[serde(default)]
    include_ignored: bool,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct HostInspectFileRequest {
    path: String,
    #[serde(default)]
    working_dir: Option<String>,
    #[serde(default = "default_large_read_bytes")]
    max_bytes: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct HostLargeReadRequest {
    #[schemars(description = "File path on the Windows host.")]
    path: String,
    #[serde(default)]
    #[schemars(
        description = "Working directory used to resolve relative Windows host paths. Defaults to the configured host working directory."
    )]
    working_dir: Option<String>,
    #[serde(default)]
    #[schemars(description = "Starting byte offset within the file.")]
    offset_bytes: u64,
    #[serde(default = "default_large_read_bytes")]
    #[schemars(description = "Maximum raw bytes to return from that offset.")]
    max_bytes: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct HostLargeWriteRequest {
    #[schemars(description = "File path on the Windows host.")]
    path: String,
    #[serde(default)]
    #[schemars(
        description = "Working directory used to resolve relative Windows host paths. Defaults to the configured host working directory."
    )]
    working_dir: Option<String>,
    #[serde(default)]
    #[schemars(
        description = "Optional UTF-8 text payload to write. Provide either content or content_base64."
    )]
    content: Option<String>,
    #[serde(default)]
    #[schemars(
        description = "Base64-encoded raw bytes to write exactly as provided. Provide either content or content_base64."
    )]
    content_base64: Option<String>,
    #[serde(default)]
    #[schemars(description = "Append to the file instead of overwriting it.")]
    append: bool,
    #[serde(default = "default_true")]
    #[schemars(description = "Create parent directories if they do not exist.")]
    create_dirs: bool,
    #[serde(default)]
    #[schemars(
        description = "Optional expected SHA-256 of the decoded payload for end-to-end verification."
    )]
    expected_sha256: Option<String>,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct RunProgramRequest {
    #[schemars(
        description = "Executable name from DEVBOX_PROGRAM_ALLOWLIST; no shell syntax is interpreted."
    )]
    program: String,
    #[serde(default)]
    #[schemars(description = "Argument vector passed directly to the executable.")]
    args: Vec<String>,
    #[serde(default)]
    #[schemars(description = "Working directory; defaults to the selected Devbox workspace.")]
    working_dir: String,
    #[serde(default = "default_sync_timeout")]
    #[schemars(description = "Synchronous timeout in seconds (1-90).")]
    timeout_seconds: u64,
    #[serde(default)]
    #[schemars(description = "Execution user; primarily useful for Docker mode.")]
    user: String,
    #[serde(default = "default_output_mode")]
    #[schemars(description = "Output shaping mode: head, tail, or summary.")]
    output_mode: String,
    #[serde(default)]
    #[schemars(description = "Maximum characters returned per output stream.")]
    max_output_chars: Option<usize>,
    #[serde(default)]
    #[schemars(
        description = "Optional maximum lines returned per stream; 0 disables line limiting."
    )]
    max_output_lines: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct RunShellRequest {
    #[schemars(description = "Shell command to execute in the selected Devbox runtime.")]
    command: String,
    #[serde(default)]
    working_dir: String,
    #[serde(default = "default_sync_timeout")]
    timeout_seconds: u64,
    #[serde(default)]
    user: String,
    #[serde(default = "default_output_mode")]
    output_mode: String,
    #[serde(default)]
    max_output_chars: Option<usize>,
    #[serde(default)]
    max_output_lines: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct HostShellToolRequest {
    command: String,
    #[serde(default)]
    working_dir: String,
    #[serde(default = "default_sync_timeout")]
    timeout_seconds: u64,
    #[serde(default = "default_output_mode")]
    output_mode: String,
    #[serde(default)]
    max_output_chars: Option<usize>,
    #[serde(default)]
    max_output_lines: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct HostProgramToolRequest {
    program: String,
    #[serde(default)]
    args: Vec<String>,
    #[serde(default)]
    working_dir: String,
    #[serde(default = "default_sync_timeout")]
    timeout_seconds: u64,
    #[serde(default = "default_output_mode")]
    output_mode: String,
    #[serde(default)]
    max_output_chars: Option<usize>,
    #[serde(default)]
    max_output_lines: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct CaptureDisplayRequest {
    #[serde(default = "default_capture_quality")]
    #[schemars(
        description = "Requested image quality from 1 through 100. Native lossless PNG backends record but do not apply JPEG quality."
    )]
    quality: u8,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct CaptureWindowRequest {
    #[schemars(
        description = "Host process ID whose visible application window should be captured."
    )]
    pid: u64,
    #[serde(default = "default_capture_quality")]
    #[schemars(description = "Requested image quality from 1 through 100.")]
    quality: u8,
    #[serde(default = "default_true")]
    #[schemars(
        description = "Also consider visible windows owned by child processes, useful for launchers, browsers, emulators, and multi-process GUI applications."
    )]
    include_process_tree: bool,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct StartShellRequest {
    #[schemars(description = "Shell command to run as a detached Devbox job.")]
    command: String,
    #[serde(default)]
    working_dir: String,
    #[serde(default = "default_async_timeout")]
    timeout_seconds: u64,
    #[serde(default)]
    user: String,
    #[serde(default)]
    read_only: bool,
    #[serde(default = "default_resource_class")]
    resource_class: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct StartProgramRequest {
    #[schemars(
        description = "Executable name from DEVBOX_PROGRAM_ALLOWLIST; no shell syntax is interpreted."
    )]
    program: String,
    #[serde(default)]
    args: Vec<String>,
    #[serde(default)]
    input: Option<String>,
    #[serde(default)]
    working_dir: String,
    #[serde(default = "default_async_timeout")]
    #[schemars(description = "Detached job timeout in seconds (1-86400).")]
    timeout_seconds: u64,
    #[serde(default)]
    user: String,
    #[serde(default = "default_resource_class")]
    #[schemars(description = "Resource class: auto, watch, light, or heavy.")]
    resource_class: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct JobStatusRequest {
    job_id: String,
    #[serde(default)]
    #[schemars(description = "Optional no-slot long-poll duration in seconds (0-85).")]
    wait_seconds: u64,
    #[serde(default = "default_true")]
    terminal_only: bool,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct JobLogsRequest {
    job_id: String,
    #[serde(default = "default_job_log_chars")]
    max_chars: usize,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct JobCancelRequest {
    job_id: String,
}

struct ShellRenderContext {
    output_mode: String,
    max_chars: usize,
    max_lines: usize,
    error_max_chars: usize,
    queue_wait_ms: u64,
    slot: Option<usize>,
}

struct ProgramRenderContext {
    output_mode: String,
    max_chars: usize,
    max_lines: usize,
    error_max_chars: usize,
    queue_wait_ms: u64,
    slot: Option<usize>,
}

const fn default_sync_timeout() -> u64 {
    90
}
const fn default_async_timeout() -> u64 {
    7_200
}
const fn default_job_log_chars() -> usize {
    20_000
}
const fn default_capture_quality() -> u8 {
    85
}
fn default_output_mode() -> String {
    "tail".to_owned()
}
fn default_resource_class() -> String {
    "auto".to_owned()
}

const fn default_true() -> bool {
    true
}
const fn default_poll_ms() -> u64 {
    250
}
const fn default_large_read_bytes() -> usize {
    262_144
}
const fn default_read_bytes() -> usize {
    65_536
}
const fn default_list_depth() -> usize {
    4
}
const fn default_list_entries() -> usize {
    5_000
}
const fn default_list_timeout() -> u64 {
    30
}
fn default_excluded_directories() -> Vec<String> {
    [
        ".git",
        "node_modules",
        ".cache",
        ".venv",
        "venv",
        "__pycache__",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect()
}
fn default_search_glob() -> String {
    "*".to_owned()
}
const fn default_search_matches() -> usize {
    200
}
const fn default_search_depth() -> usize {
    12
}
const fn default_search_file_bytes() -> u64 {
    2 * 1024 * 1024
}
const fn default_search_timeout() -> u64 {
    30
}

#[tool_router(router = tool_router)]
impl DevboxMcp {
    #[tool(
        name = "devbox_github_auth_status",
        description = "Confirm whether the selected Devbox runtime is authenticated to GitHub and which global git identity is configured.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_github_auth_status(&self, cancellation: CancellationToken) -> CallToolResult {
        match self.github_auth.status(cancellation).await {
            Ok(data) => ToolEnvelope::success(
                format!(
                    "Fetched {} GitHub auth status.",
                    self.config.runtime_label()
                ),
                serde_json::to_value(data).ok(),
            ),
            Err(error) => render_anyhow_tool_error(&error, self.config.command_output_limit_chars),
        }
    }

    #[tool(
        name = "devbox_sync_github_auth_from_host",
        description = "Copy the existing host GitHub CLI login and global git identity into the selected Devbox runtime without exposing the token in MCP output.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_sync_github_auth_from_host(
        &self,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        match self.github_auth.sync_from_host(cancellation).await {
            Ok(result) => ToolEnvelope::success(
                format!(
                    "Synced the host GitHub CLI authentication into the {}.",
                    self.config.runtime_label()
                ),
                Some(json!({
                    "statusSummary": result.status.status_summary,
                    "userName": result.status.user_name,
                    "userEmail": result.status.user_email,
                    "hostUserName": if result.host_user_name.is_empty() { Value::Null } else { Value::String(result.host_user_name) },
                    "hostUserEmail": if result.host_user_email.is_empty() { Value::Null } else { Value::String(result.host_user_email) },
                })),
            ),
            Err(error) => render_anyhow_tool_error(&error, self.config.command_output_limit_chars),
        }
    }

    fn configured_tool(&self, mut tool: rmcp::model::Tool) -> rmcp::model::Tool {
        let mut schema = (*tool.input_schema).clone();
        crate::schema_parity::configure_tool_input_schema(
            tool.name.as_ref(),
            &mut schema,
            &self.config,
        );
        tool.input_schema = Arc::new(schema);
        crate::schema_parity::configure_tool_output_schema(&mut tool);
        crate::schema_parity::configure_tool_metadata(&mut tool, &self.config);
        tool
    }

    #[tool(
        name = "devbox_status",
        description = "Use this when you need the current state of the selected Devbox runtime.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_status(&self, cancellation: CancellationToken) -> CallToolResult {
        let info = match self.lifecycle.status(cancellation.child_token()).await {
            Ok(info) => info,
            Err(error) => {
                return ToolEnvelope::error(
                    format!(
                        "Failed to fetch {} status: {error}",
                        self.config.runtime_label()
                    ),
                    None,
                );
            }
        };
        let execution = match self.scheduler.snapshot().await {
            Ok(snapshot) => snapshot,
            Err(error) => {
                return ToolEnvelope::error(
                    format!(
                        "Failed to fetch {} status: {error}",
                        self.config.runtime_label()
                    ),
                    None,
                );
            }
        };
        let (guardian, startup, job_maintenance) = tokio::join!(
            read_guardian_status_snapshot(&self.config),
            read_json_snapshot(
                self.config
                    .project_root
                    .join("run")
                    .join("startup-state.json")
            ),
            read_json_snapshot(
                self.config
                    .project_root
                    .join("run")
                    .join("job-maintenance.json")
            ),
        );
        let mut data = info.as_object().cloned().unwrap_or_default();
        data.insert(
            "hostWorkspacePath".to_owned(),
            json!(self.config.host_workspace_path),
        );
        data.insert(
            "devboxWorkspacePath".to_owned(),
            json!(self.config.devbox_workspace_path),
        );
        data.insert(
            "hostExecEnabled".to_owned(),
            json!(self.config.host_exec_enabled),
        );
        data.insert("guardian".to_owned(), guardian.unwrap_or(Value::Null));
        data.insert("startup".to_owned(), startup.unwrap_or(Value::Null));
        data.insert(
            "jobMaintenance".to_owned(),
            job_maintenance.unwrap_or(Value::Null),
        );
        data.insert("execution".to_owned(), json!(execution));
        data.insert("performance".to_owned(), self.performance.snapshot());
        data.insert(
            "activeRequests".to_owned(),
            json!(self.active_requests.active_count()),
        );
        #[cfg(windows)]
        data.insert(
            "processProbe".to_owned(),
            crate::windows_process::metrics_snapshot(),
        );
        if info.get("running").and_then(Value::as_bool) == Some(true) {
            let versions = self.runtime.cached_versions().await;
            data.insert(
                "versions".to_owned(),
                versions.clone().map_or(Value::Null, |value| json!(value)),
            );
            data.insert("versionsCached".to_owned(), json!(versions.is_some()));
        }
        ToolEnvelope::success(
            format!("Fetched {} status.", self.config.runtime_label()),
            Some(Value::Object(data)),
        )
    }

    async fn run_shell_internal(
        &self,
        request: RunShellRequest,
        read_only: bool,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.command.trim().is_empty() {
            return ToolEnvelope::error("command must not be empty", None);
        }
        if !(1..=90).contains(&request.timeout_seconds) {
            return ToolEnvelope::error("timeout_seconds must be between 1 and 90", None);
        }
        let max_output_chars = self.config.command_output_chars(request.max_output_chars);
        if max_output_chars < 100 || max_output_chars > self.config.command_output_limit_chars {
            return ToolEnvelope::error(
                format!(
                    "max_output_chars must be between 100 and {}",
                    self.config.command_output_limit_chars
                ),
                None,
            );
        }
        let mut acquire = AcquireRequest::interactive(if read_only {
            "devbox_exec_readonly"
        } else {
            "devbox_exec"
        });
        acquire.queue_timeout = Some(Duration::from_millis(self.config.exec_queue_timeout_ms));
        let mut lease = match self.scheduler.acquire(acquire, &cancellation).await {
            Ok(lease) => lease,
            Err(error) => return ToolEnvelope::error(error.to_string(), None),
        };
        let working_dir = if request.working_dir.trim().is_empty() {
            self.config.devbox_workspace_path.clone()
        } else {
            PathBuf::from(request.working_dir.trim())
        };
        let working_dir_display = working_dir.to_string_lossy().into_owned();
        let result = self
            .runtime
            .run_shell(
                ShellRequest {
                    command: request.command,
                    working_dir,
                    timeout: Duration::from_secs(request.timeout_seconds),
                    user: request.user,
                    max_capture_chars: None,
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await;
        if let Err(error) = lease.release().await {
            return ToolEnvelope::error(
                format!(
                    "Shell command completed but its execution slot could not be released: {error}"
                ),
                None,
            );
        }
        let summary = if read_only {
            format!(
                "Ran a read-only shell command in the {} at {working_dir_display}.",
                self.config.runtime_label()
            )
        } else {
            format!(
                "Ran a shell command in the {} at {working_dir_display}.",
                self.config.runtime_label()
            )
        };
        render_shell_result(
            summary,
            &ShellRenderContext {
                output_mode: request.output_mode,
                max_chars: max_output_chars,
                max_lines: request.max_output_lines,
                error_max_chars: self.config.command_output_limit_chars,
                queue_wait_ms: lease.queue_wait_ms,
                slot: lease.slot,
            },
            result,
        )
    }

    async fn run_host_shell_internal(
        &self,
        request: HostShellToolRequest,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if !self.config.host_exec_enabled {
            return ToolEnvelope::error("Host execution is disabled.", None);
        }
        if request.command.trim().is_empty() {
            return ToolEnvelope::error("command must not be empty", None);
        }
        if !(1..=90).contains(&request.timeout_seconds) {
            return ToolEnvelope::error("timeout_seconds must be between 1 and 90", None);
        }
        let max_output_chars = self.config.command_output_chars(request.max_output_chars);
        if max_output_chars < 100
            || max_output_chars > self.config.command_output_limit_chars
            || request.max_output_lines > 10_000
        {
            return ToolEnvelope::error("Invalid host output bounds.", None);
        }
        let mut acquire = AcquireRequest::interactive("host_exec");
        acquire.queue_timeout = Some(Duration::from_millis(self.config.exec_queue_timeout_ms));
        let mut lease = match self.scheduler.acquire(acquire, &cancellation).await {
            Ok(lease) => lease,
            Err(error) => return ToolEnvelope::error(error.to_string(), None),
        };
        let working_dir = if request.working_dir.trim().is_empty() {
            self.config.host_default_workdir.clone()
        } else {
            PathBuf::from(request.working_dir.trim())
        };
        let working_dir_display = working_dir.to_string_lossy().into_owned();
        let result = self
            .runtime
            .run_host_shell_only(
                ShellRequest {
                    command: request.command,
                    working_dir,
                    timeout: Duration::from_secs(request.timeout_seconds.saturating_add(5)),
                    user: String::new(),
                    max_capture_chars: None,
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await;
        if let Err(error) = lease.release().await {
            return ToolEnvelope::error(
                format!(
                    "Host command completed but its execution slot could not be released: {error}"
                ),
                None,
            );
        }
        render_shell_result(
            format!(
                "Ran a {} command in {working_dir_display}.",
                host_command_title(&self.config).to_lowercase()
            ),
            &ShellRenderContext {
                output_mode: request.output_mode,
                max_chars: max_output_chars,
                max_lines: request.max_output_lines,
                error_max_chars: self.config.command_output_limit_chars,
                queue_wait_ms: lease.queue_wait_ms,
                slot: lease.slot,
            },
            result,
        )
    }

    async fn run_host_program_internal(
        &self,
        request: HostProgramToolRequest,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if !self.config.host_exec_enabled {
            return ToolEnvelope::error("Host execution is disabled.", None);
        }
        if request.program.trim().is_empty() {
            return ToolEnvelope::error("program must not be empty", None);
        }
        if !(1..=90).contains(&request.timeout_seconds) {
            return ToolEnvelope::error("timeout_seconds must be between 1 and 90", None);
        }
        let max_output_chars = self.config.command_output_chars(request.max_output_chars);
        if max_output_chars < 100
            || max_output_chars > self.config.command_output_limit_chars
            || request.max_output_lines > 10_000
        {
            return ToolEnvelope::error("Invalid host output bounds.", None);
        }
        let mut acquire =
            AcquireRequest::interactive(format!("host_run_program:{}", request.program.trim()));
        acquire.queue_timeout = Some(Duration::from_millis(self.config.exec_queue_timeout_ms));
        let mut lease = match self.scheduler.acquire(acquire, &cancellation).await {
            Ok(lease) => lease,
            Err(error) => return ToolEnvelope::error(error.to_string(), None),
        };
        let working_dir = if request.working_dir.trim().is_empty() {
            self.config.host_default_workdir.clone()
        } else {
            PathBuf::from(request.working_dir.trim())
        };
        let result = self
            .runtime
            .run_host_program_only(
                ProgramRequest {
                    program: request.program.trim().to_owned(),
                    args: request.args,
                    input: None,
                    working_dir,
                    timeout: Duration::from_secs(request.timeout_seconds.saturating_add(5)),
                    user: String::new(),
                    max_capture_chars: None,
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await;
        if let Err(error) = lease.release().await {
            return ToolEnvelope::error(
                format!(
                    "Host program completed but its execution slot could not be released: {error}"
                ),
                None,
            );
        }
        render_program_result(
            format!(
                "Ran {} on the {}.",
                request.program,
                host_title(&self.config).to_lowercase()
            ),
            &ProgramRenderContext {
                output_mode: request.output_mode,
                max_chars: max_output_chars,
                max_lines: request.max_output_lines,
                error_max_chars: self.config.command_output_limit_chars,
                queue_wait_ms: lease.queue_wait_ms,
                slot: lease.slot,
            },
            result,
        )
    }

    #[tool(
        name = "devbox_wait",
        description = "Wait without consuming an execution process or slot.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_wait(
        &self,
        Parameters(request): Parameters<WaitRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        let upper = self.config.max_wait_seconds.min(85.0);
        if !request.seconds.is_finite() || request.seconds < 0.05 || request.seconds > upper {
            return ToolEnvelope::error(format!("seconds must be between 0.05 and {upper}"), None);
        }
        let started = Instant::now();
        tokio::select! {
            () = tokio::time::sleep(Duration::from_secs_f64(request.seconds)) => ToolEnvelope::success(
                format!("Waited {} seconds without an execution process.", request.seconds),
                Some(json!({
                    "waited_ms": u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
                    "reason": if request.reason.is_empty() { serde_json::Value::Null } else { serde_json::Value::String(request.reason) },
                })),
            ),
            () = cancellation.cancelled() => ToolEnvelope::error("Wait was cancelled.", None),
        }
    }

    #[tool(
        name = "devbox_wait_for_file",
        description = "Host-mode filesystem condition wait without spawning a shell.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_wait_for_file(
        &self,
        Parameters(request): Parameters<WaitForFileRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if self.config.runtime_mode == RuntimeMode::Docker {
            return ToolEnvelope::error(
                "devbox_wait_for_file is a no-subprocess host-mode primitive; use devbox_wait in Docker mode.",
                None,
            );
        }
        let requested_path = request.path.clone();
        match wait_for_path(
            request,
            self.config.max_wait_seconds.min(85.0),
            cancellation,
        )
        .await
        {
            Ok(data) => {
                let summary = if data["conditionMet"].as_bool().unwrap_or(false) {
                    format!("File condition satisfied for {requested_path}.")
                } else {
                    format!("Timed out waiting for file condition at {requested_path}.")
                };
                ToolEnvelope::success(summary, Some(data))
            }
            Err(message) => ToolEnvelope::error(message, None),
        }
    }

    #[tool(
        name = "devbox_start",
        description = "Bring the selected Devbox runtime online. Host mode stays in the current server process; Docker mode starts or creates the configured container.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_start(&self, cancellation: CancellationToken) -> CallToolResult {
        self.render_lifecycle(LifecycleAction::Start, cancellation)
            .await
    }

    #[tool(
        name = "devbox_stop",
        description = "Stop the selected Devbox runtime without deleting its workspace. Host mode returns launcher guidance instead of terminating the MCP process.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_stop(&self, cancellation: CancellationToken) -> CallToolResult {
        self.render_lifecycle(LifecycleAction::Stop, cancellation)
            .await
    }

    #[tool(
        name = "devbox_restart",
        description = "Restart the selected Devbox runtime. Host mode returns launcher guidance; Docker mode restarts or creates the configured container.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_restart(&self, cancellation: CancellationToken) -> CallToolResult {
        self.render_lifecycle(LifecycleAction::Restart, cancellation)
            .await
    }

    #[tool(
        name = "devbox_recreate",
        description = "Rebuild the selected Devbox backend while preserving its workspace. Docker recreate uses rename/create/rollback and legacy /tmp migration semantics.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_recreate(&self, cancellation: CancellationToken) -> CallToolResult {
        self.render_lifecycle(LifecycleAction::Recreate, cancellation)
            .await
    }

    async fn render_lifecycle(
        &self,
        action: LifecycleAction,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        let should_run = action != LifecycleAction::Stop;
        let source = match action {
            LifecycleAction::Start => "src/server.js:devbox_start",
            LifecycleAction::Stop => "src/server.js:devbox_stop",
            LifecycleAction::Restart => "src/server.js:devbox_restart",
            LifecycleAction::Recreate => "src/server.js:devbox_recreate",
        };
        if let Err(error) = self
            .lifecycle
            .set_guardian_desired_state(should_run, source)
            .await
        {
            return ToolEnvelope::error(
                format!(
                    "Failed to {} the {}: {error}",
                    action.as_str(),
                    self.config.runtime_label()
                ),
                None,
            );
        }
        match self.lifecycle.control(action, cancellation).await {
            Ok(data) => {
                let docker = self.config.runtime_mode == RuntimeMode::Docker;
                let name = data["name"].as_str().unwrap_or("devbox");
                let summary = if docker {
                    match action {
                        LifecycleAction::Start => format!("Docker Devbox {name} is running."),
                        LifecycleAction::Stop => format!("Docker Devbox {name} is stopped."),
                        LifecycleAction::Restart => {
                            format!("Docker Devbox {name} has been restarted.")
                        }
                        LifecycleAction::Recreate => {
                            format!("Docker Devbox {name} has been recreated.")
                        }
                    }
                } else if action == LifecycleAction::Start {
                    format!(
                        "{} Host Devbox is ready in the current server process.",
                        self.config.platform.display_name
                    )
                } else {
                    data["controlMessage"].as_str().map_or_else(
                        || {
                            format!(
                                "{} Host Devbox {} is managed by the launcher command.",
                                self.config.platform.display_name,
                                action.as_str()
                            )
                        },
                        str::to_owned,
                    )
                };
                ToolEnvelope::success(summary, Some(data))
            }
            Err(error) => ToolEnvelope::error(
                format!(
                    "Failed to {} the {}: {error}",
                    action.as_str(),
                    self.config.runtime_label()
                ),
                None,
            ),
        }
    }

    #[tool(
        name = "devbox_exec_readonly",
        description = "Run an inspection-only shell command in the selected Devbox runtime. This is an advisory read-only surface; prefer direct structured tools when possible.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_exec_readonly(
        &self,
        Parameters(request): Parameters<RunShellRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.run_shell_internal(request, true, cancellation).await
    }

    #[tool(
        name = "devbox_exec",
        description = "Run a shell command in the selected Devbox runtime. Prefer devbox_run_program when shell parsing is unnecessary.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_exec(
        &self,
        Parameters(request): Parameters<RunShellRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.run_shell_internal(request, false, cancellation).await
    }

    #[tool(
        name = "devbox_exec_start",
        description = "Start a shell command as a detached Devbox job and return immediately with a job id.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_exec_start(
        &self,
        Parameters(request): Parameters<StartShellRequest>,
    ) -> CallToolResult {
        if request.command.trim().is_empty() {
            return ToolEnvelope::error("command must not be empty", None);
        }
        if !(1..=86_400).contains(&request.timeout_seconds) {
            return ToolEnvelope::error("timeout_seconds must be between 1 and 86400", None);
        }
        match self
            .jobs
            .start_shell(crate::job_manager::StartShellJob {
                command: request.command,
                working_dir: request.working_dir,
                timeout: Duration::from_secs(request.timeout_seconds),
                user: request.user,
                read_only: request.read_only,
                resource_class: request.resource_class,
            })
            .await
        {
            Ok(job) => ToolEnvelope::success(
                format!(
                    "Started background {} job {}.",
                    self.config.runtime_label(),
                    job.id
                ),
                serde_json::to_value(job).ok(),
            ),
            Err(error) => {
                ToolEnvelope::error(format!("Failed to start detached shell job: {error}"), None)
            }
        }
    }

    #[tool(
        name = "devbox_run_program",
        description = "Run one allowlisted executable directly without a shell. Prefer this for structured tools such as git, gh, rg, node, and python.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_run_program(
        &self,
        Parameters(request): Parameters<RunProgramRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.program.trim().is_empty() {
            return ToolEnvelope::error("program must not be empty", None);
        }
        if !(1..=90).contains(&request.timeout_seconds) {
            return ToolEnvelope::error("timeout_seconds must be between 1 and 90", None);
        }
        let max_output_chars = self.config.command_output_chars(request.max_output_chars);
        if max_output_chars < 100 || max_output_chars > self.config.command_output_limit_chars {
            return ToolEnvelope::error(
                format!(
                    "max_output_chars must be between 100 and {}",
                    self.config.command_output_limit_chars
                ),
                None,
            );
        }
        let mut acquire =
            AcquireRequest::interactive(format!("devbox_run_program:{}", request.program.trim()));
        acquire.queue_timeout = Some(Duration::from_millis(self.config.exec_queue_timeout_ms));
        let mut lease = match self.scheduler.acquire(acquire, &cancellation).await {
            Ok(lease) => lease,
            Err(error) => return ToolEnvelope::error(error.to_string(), None),
        };
        let working_dir = if request.working_dir.trim().is_empty() {
            self.config.devbox_workspace_path.clone()
        } else {
            PathBuf::from(request.working_dir.trim())
        };
        let result = self
            .runtime
            .run_program(
                ProgramRequest {
                    program: request.program.trim().to_owned(),
                    args: request.args,
                    input: None,
                    working_dir,
                    timeout: Duration::from_secs(request.timeout_seconds),
                    user: request.user,
                    max_capture_chars: None,
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await;
        if let Err(error) = lease.release().await {
            return ToolEnvelope::error(
                format!("Program completed but its execution slot could not be released: {error}"),
                None,
            );
        }
        render_program_result(
            format!(
                "Ran {} directly in the {}.",
                request.program,
                self.config.runtime_label()
            ),
            &ProgramRenderContext {
                output_mode: request.output_mode,
                max_chars: max_output_chars,
                max_lines: request.max_output_lines,
                error_max_chars: self.config.command_output_limit_chars,
                queue_wait_ms: lease.queue_wait_ms,
                slot: lease.slot,
            },
            result,
        )
    }

    #[tool(
        name = "devbox_run_program_start",
        description = "Start one allowlisted executable as a detached Devbox job without shell parsing.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_run_program_start(
        &self,
        Parameters(request): Parameters<StartProgramRequest>,
    ) -> CallToolResult {
        if request.program.trim().is_empty() {
            return ToolEnvelope::error("program must not be empty", None);
        }
        if !(1..=86_400).contains(&request.timeout_seconds) {
            return ToolEnvelope::error("timeout_seconds must be between 1 and 86400", None);
        }
        match self
            .jobs
            .start_program(StartProgramJob {
                program: request.program.trim().to_owned(),
                args: request.args,
                input: request.input,
                working_dir: request.working_dir,
                timeout: Duration::from_secs(request.timeout_seconds),
                user: request.user,
                resource_class: request.resource_class,
            })
            .await
        {
            Ok(job) => ToolEnvelope::success(
                format!(
                    "Started direct background {} job {}.",
                    self.config.runtime_label(),
                    job.id
                ),
                serde_json::to_value(job).ok(),
            ),
            Err(error) => ToolEnvelope::error(
                format!("Failed to start detached program job: {error}"),
                None,
            ),
        }
    }

    #[tool(
        name = "devbox_job_status",
        description = "Get detached job status, optionally long-polling without consuming an execution slot.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_job_status(
        &self,
        Parameters(request): Parameters<JobStatusRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        let max_wait_seconds = self.config.max_wait_seconds.min(85.0);
        if Duration::from_secs(request.wait_seconds) > Duration::from_secs_f64(max_wait_seconds) {
            return ToolEnvelope::error(
                format!("wait_seconds must be between 0 and {max_wait_seconds}"),
                None,
            );
        }
        let result = if request.wait_seconds == 0 {
            self.jobs.store().get_status(&request.job_id).await
        } else {
            self.jobs
                .store()
                .wait_status(
                    &request.job_id,
                    Duration::from_secs(request.wait_seconds),
                    request.terminal_only,
                    Duration::from_millis(500),
                    &cancellation,
                )
                .await
        };
        match result {
            Ok(status) => {
                let status_name = status
                    .get("status")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown");
                ToolEnvelope::success(
                    format!("Background job {} is {status_name}.", request.job_id),
                    Some(status),
                )
            }
            Err(error) => ToolEnvelope::error(
                format!("Failed to fetch detached job {}: {error}", request.job_id),
                None,
            ),
        }
    }

    #[tool(
        name = "devbox_job_logs",
        description = "Read bounded stdout/stderr tails for a detached Devbox job, including rotated segments.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_job_logs(
        &self,
        Parameters(request): Parameters<JobLogsRequest>,
    ) -> CallToolResult {
        if !(100..=100_000).contains(&request.max_chars) {
            return ToolEnvelope::error("max_chars must be between 100 and 100000", None);
        }
        match self
            .jobs
            .store()
            .logs(&request.job_id, request.max_chars)
            .await
        {
            Ok(logs) => ToolEnvelope::success(
                format!("Read logs for background job {}.", request.job_id),
                serde_json::to_value(logs).ok(),
            ),
            Err(error) => ToolEnvelope::error(
                format!(
                    "Failed to fetch logs for detached job {}: {error}",
                    request.job_id
                ),
                None,
            ),
        }
    }

    #[tool(
        name = "devbox_job_cancel",
        description = "Cancel a detached Devbox job and terminate its runner process tree.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_job_cancel(
        &self,
        Parameters(request): Parameters<JobCancelRequest>,
    ) -> CallToolResult {
        match self.jobs.store().cancel(&request.job_id).await {
            Ok(status) => {
                let status_name = status
                    .get("status")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown");
                ToolEnvelope::success(
                    format!("Background job {} is {status_name}.", request.job_id),
                    Some(status),
                )
            }
            Err(error) => ToolEnvelope::error(
                format!("Failed to cancel detached job {}: {error}", request.job_id),
                None,
            ),
        }
    }

    #[tool(
        name = "devbox_list_files",
        description = "List files inside the selected Devbox runtime with bounded recursion and pruning.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_list_files(
        &self,
        Parameters(request): Parameters<DevboxListFilesRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if !(1..=20).contains(&request.max_depth)
            || !(1..=50_000).contains(&request.max_entries)
            || !(1..=300).contains(&request.timeout_seconds)
            || request.exclude_directories.len() > 32
        {
            return ToolEnvelope::error("Invalid file-list bounds.", None);
        }
        let path = if request.path.trim().is_empty() {
            self.config
                .devbox_workspace_path
                .to_string_lossy()
                .into_owned()
        } else {
            request.path
        };
        let result = match self.config.runtime_mode {
            RuntimeMode::Host => self
                .files
                .list(
                    &ListOptions {
                        path: PathBuf::from(&path),
                        recursive: request.recursive,
                        max_depth: request.max_depth,
                        max_entries: request.max_entries,
                        timeout: Duration::from_secs(request.timeout_seconds),
                        exclude_directories: request.exclude_directories,
                    },
                    &cancellation,
                )
                .await
                .map_err(|error| error.to_string()),
            RuntimeMode::Docker => self
                .docker_files
                .list(
                    &self.config,
                    &DockerListOptions {
                        path: path.clone(),
                        recursive: request.recursive,
                        max_depth: request.max_depth,
                        max_entries: request.max_entries,
                        timeout: Duration::from_secs(request.timeout_seconds),
                        exclude_directories: request.exclude_directories,
                    },
                    cancellation,
                )
                .await
                .map_err(|error| error.to_string()),
        };
        render_file_process_result(
            format!("Listed files in {path}."),
            result,
            self.config.command_output_limit_chars,
        )
    }

    #[tool(
        name = "devbox_read_file",
        description = "Read bounded UTF-8 text content from a regular file inside the selected Devbox runtime.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_read_file(
        &self,
        Parameters(request): Parameters<DevboxReadFileRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.path.trim().is_empty() {
            return ToolEnvelope::error("path must not be empty", None);
        }
        if request.max_bytes == 0 || request.max_bytes > self.config.max_mcp_transfer_chars {
            return ToolEnvelope::error(
                format!(
                    "max_bytes must be between 1 and {}",
                    self.config.max_mcp_transfer_chars
                ),
                None,
            );
        }
        let result = match self.config.runtime_mode {
            RuntimeMode::Host => self
                .files
                .read_text(PathBuf::from(&request.path).as_path(), request.max_bytes)
                .await
                .map_err(|error| error.to_string()),
            RuntimeMode::Docker => self
                .docker_files
                .read_text(&self.config, &request.path, request.max_bytes, cancellation)
                .await
                .map_err(|error| error.to_string()),
        };
        render_file_process_result(
            format!(
                "Read {} from the {}.",
                request.path,
                self.config.runtime_label()
            ),
            result,
            self.config.command_output_limit_chars,
        )
    }

    #[tool(
        name = "devbox_read_large_file",
        description = "Read an exact byte range from a larger file inside the selected Devbox runtime.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_read_large_file(
        &self,
        Parameters(request): Parameters<DevboxLargeReadRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.path.trim().is_empty() {
            return ToolEnvelope::error("path must not be empty", None);
        }
        if request.max_bytes == 0 || request.max_bytes > self.config.max_mcp_transfer_chars {
            return ToolEnvelope::error(
                format!(
                    "max_bytes must be between 1 and {}",
                    self.config.max_mcp_transfer_chars
                ),
                None,
            );
        }
        let result = match self.config.runtime_mode {
            RuntimeMode::Host => self
                .files
                .read_large(
                    PathBuf::from(&request.path).as_path(),
                    request.offset_bytes,
                    request.max_bytes,
                )
                .await
                .map_err(|error| error.to_string()),
            RuntimeMode::Docker => self
                .docker_files
                .read_large(
                    &self.config,
                    &request.path,
                    request.offset_bytes,
                    request.max_bytes,
                    cancellation,
                )
                .await
                .map_err(|error| error.to_string()),
        };
        match result {
            Ok(data) => {
                let summary = format!(
                    "Read {} from byte {} in the {}.",
                    request.path,
                    request.offset_bytes,
                    self.config.runtime_label()
                );
                let text = large_read_text(&summary, &data);
                ToolEnvelope::success_with_text(summary, serde_json::to_value(data).ok(), text)
            }
            Err(error) => {
                ToolEnvelope::error(format!("Failed to read {}: {error}", request.path), None)
            }
        }
    }

    #[tool(
        name = "devbox_write_file",
        description = "Create, overwrite, or append UTF-8 text inside the selected Devbox runtime.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_write_file(
        &self,
        Parameters(request): Parameters<DevboxWriteFileRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.path.trim().is_empty() {
            return ToolEnvelope::error("path must not be empty", None);
        }
        let result = match self.config.runtime_mode {
            RuntimeMode::Host => self
                .files
                .write_text(
                    PathBuf::from(&request.path).as_path(),
                    &request.content,
                    request.append,
                    request.create_dirs,
                )
                .await
                .map_err(|error| error.to_string()),
            RuntimeMode::Docker => self
                .docker_files
                .write_text(
                    &self.config,
                    &request.path,
                    &request.content,
                    request.append,
                    request.create_dirs,
                    cancellation,
                )
                .await
                .map_err(|error| error.to_string()),
        };
        let summary = if request.append {
            format!(
                "Appended text to {} in the {}.",
                request.path,
                self.config.runtime_label()
            )
        } else {
            format!(
                "Wrote {} in the {}.",
                request.path,
                self.config.runtime_label()
            )
        };
        render_file_process_result(summary, result, self.config.command_output_limit_chars)
    }

    #[tool(
        name = "devbox_write_large_file",
        description = "Create, overwrite, or append exact bytes inside the selected Devbox runtime with post-write verification.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_write_large_file(
        &self,
        Parameters(request): Parameters<DevboxLargeWriteRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.path.trim().is_empty() {
            return ToolEnvelope::error("path must not be empty", None);
        }
        let content_base64 = match normalize_large_write_payload(
            request.content.as_deref(),
            request.content_base64.as_deref(),
        ) {
            Ok(value) => value,
            Err(message) => return ToolEnvelope::error(message, None),
        };
        let result = match self.config.runtime_mode {
            RuntimeMode::Host => self
                .files
                .write_large(
                    PathBuf::from(&request.path).as_path(),
                    &content_base64,
                    request.append,
                    request.create_dirs,
                    request.expected_sha256.as_deref(),
                )
                .await
                .map_err(|error| error.to_string()),
            RuntimeMode::Docker => self
                .docker_files
                .write_large(
                    &self.config,
                    &request.path,
                    &content_base64,
                    request.append,
                    request.create_dirs,
                    request.expected_sha256.as_deref(),
                    cancellation,
                )
                .await
                .map_err(|error| error.to_string()),
        };
        match result {
            Ok(data) => {
                let summary = if request.append {
                    format!(
                        "Appended large payload to {} in the {} and verified the exact bytes.",
                        request.path,
                        self.config.runtime_label()
                    )
                } else {
                    format!(
                        "Wrote large payload to {} in the {} and verified the exact bytes.",
                        request.path,
                        self.config.runtime_label()
                    )
                };
                let text = large_write_text(&summary, &data);
                ToolEnvelope::success_with_text(summary, serde_json::to_value(data).ok(), text)
            }
            Err(error) => ToolEnvelope::error(
                format!("Failed to write large payload to {}: {error}", request.path),
                None,
            ),
        }
    }

    #[tool(
        name = "devbox_search_files",
        description = "Search text inside the selected Devbox runtime with bounded ripgrep-style semantics and a native host fallback.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_search_files(
        &self,
        Parameters(request): Parameters<SearchFilesRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if request.pattern.is_empty() {
            return ToolEnvelope::error("pattern must not be empty", None);
        }
        if !(1..=5_000).contains(&request.max_matches)
            || !(1..=50).contains(&request.max_depth)
            || !(1..=64 * 1024 * 1024).contains(&request.max_file_bytes)
            || !(1..=300).contains(&request.timeout_seconds)
            || request.exclude_directories.len() > 32
        {
            return ToolEnvelope::error("Invalid search bounds.", None);
        }
        let path = if request.path.trim().is_empty() {
            self.config
                .devbox_workspace_path
                .to_string_lossy()
                .into_owned()
        } else {
            request.path
        };
        let mut acquire = AcquireRequest::interactive("devbox_search_files");
        acquire.queue_timeout = Some(Duration::from_millis(self.config.exec_queue_timeout_ms));
        let mut lease = match self.scheduler.acquire(acquire, &cancellation).await {
            Ok(lease) => lease,
            Err(error) => return ToolEnvelope::error(error.to_string(), None),
        };
        let result = self
            .search
            .search(
                SearchRequest {
                    pattern: request.pattern.clone(),
                    path: path.clone(),
                    glob: request.glob,
                    case_sensitive: request.case_sensitive,
                    max_matches: request.max_matches,
                    max_depth: request.max_depth,
                    max_file_bytes: request.max_file_bytes,
                    timeout: Duration::from_secs(request.timeout_seconds),
                    exclude_directories: request.exclude_directories,
                    include_ignored: request.include_ignored,
                },
                cancellation,
            )
            .await;
        if let Err(error) = lease.release().await {
            return ToolEnvelope::error(
                format!("Search completed but its execution slot could not be released: {error}"),
                None,
            );
        }
        match result {
            Ok(result) => {
                let max_chars = self.config.max_mcp_transfer_chars.clamp(100, 65_536);
                let stdout = shape_process_output(&result.stdout, OutputMode::Tail, max_chars, 0);
                let stderr = shape_process_output(&result.stderr, OutputMode::Tail, max_chars, 0);
                ToolEnvelope::process_success(
                    format!(
                        "Searched {path} for \"{}\" inside the {}.",
                        request.pattern,
                        self.config.runtime_label()
                    ),
                    Some(json!({
                        "execution": {
                            "queue_wait_ms": lease.queue_wait_ms,
                            "slot": lease.slot,
                        },
                        "output": {
                            "mode": "tail",
                            "max_chars": max_chars,
                            "max_lines": 0,
                            "stdout_original_chars": stdout.original_chars,
                            "stderr_original_chars": stderr.original_chars,
                        }
                    })),
                    stdout.text,
                    stderr.text,
                    result.exit_code,
                    stdout.truncated || stderr.truncated,
                )
            }
            Err(error) => ToolEnvelope::error(
                format!(
                    "Failed to search {path} inside the {}: {error}",
                    self.config.runtime_label()
                ),
                Some(json!({
                    "execution": {
                        "queue_wait_ms": lease.queue_wait_ms,
                        "slot": lease.slot,
                    }
                })),
            ),
        }
    }

    #[tool(
        name = "windows_host_inspect_file",
        description = "Inspect exact host-file bytes, encoding/corruption signals, binary magic, and PowerShell syntax where relevant.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_inspect_file(
        &self,
        Parameters(request): Parameters<HostInspectFileRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        if !self.config.host_exec_enabled {
            return ToolEnvelope::error("Host execution is disabled.", None);
        }
        if request.path.trim().is_empty() {
            return ToolEnvelope::error("path must not be empty", None);
        }
        if request.max_bytes == 0 || request.max_bytes > self.config.max_mcp_transfer_chars {
            return ToolEnvelope::error(
                format!(
                    "max_bytes must be between 1 and {}",
                    self.config.max_mcp_transfer_chars
                ),
                None,
            );
        }
        let working_dir = request
            .working_dir
            .as_deref()
            .filter(|value| !value.trim().is_empty())
            .map_or_else(|| self.config.host_default_workdir.clone(), PathBuf::from);
        let resolved_path = match resolve_host_file_path(
            &request.path,
            request.working_dir.as_deref(),
            &self.config.host_default_workdir,
        ) {
            Ok(path) => path,
            Err(message) => return ToolEnvelope::error(message, None),
        };
        match inspect_host_file(
            self.config.clone(),
            self.runtime.clone(),
            InspectFileRequest {
                path: request.path.clone(),
                working_dir,
                resolved_path: Some(resolved_path),
                max_bytes: request.max_bytes,
            },
            cancellation,
        )
        .await
        {
            Ok(data) => ToolEnvelope::success(
                format!("Inspected {} on the Windows host.", request.path),
                Some(data),
            ),
            Err(error) => ToolEnvelope::error(error.to_string(), None),
        }
    }

    #[tool(
        name = "windows_host_read_large_file",
        description = "Read an exact byte range from a Windows host file without lossy UTF-8 conversion.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_read_large_file(
        &self,
        Parameters(request): Parameters<HostLargeReadRequest>,
    ) -> CallToolResult {
        if !self.config.host_exec_enabled {
            return ToolEnvelope::error("Windows host execution is disabled.", None);
        }
        if request.max_bytes == 0 || request.max_bytes > self.config.max_mcp_transfer_chars {
            return ToolEnvelope::error(
                format!(
                    "max_bytes must be between 1 and {}",
                    self.config.max_mcp_transfer_chars
                ),
                None,
            );
        }
        let path = match resolve_host_file_path(
            &request.path,
            request.working_dir.as_deref(),
            &self.config.host_default_workdir,
        ) {
            Ok(path) => path,
            Err(message) => return ToolEnvelope::error(message, None),
        };
        match self
            .files
            .read_large(&path, request.offset_bytes, request.max_bytes)
            .await
        {
            Ok(data) => {
                let summary = format!(
                    "Read {} from byte {} on the Windows host.",
                    request.path, request.offset_bytes
                );
                let text = large_read_text(&summary, &data);
                ToolEnvelope::success_with_text(summary, serde_json::to_value(data).ok(), text)
            }
            Err(error) => ToolEnvelope::error(error.to_string(), None),
        }
    }

    #[tool(
        name = "windows_host_write_large_file",
        description = "Create, overwrite, or append exact bytes to a Windows host file with post-write verification.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_write_large_file(
        &self,
        Parameters(request): Parameters<HostLargeWriteRequest>,
    ) -> CallToolResult {
        if !self.config.host_exec_enabled {
            return ToolEnvelope::error("Windows host execution is disabled.", None);
        }
        let content_base64 = match normalize_large_write_payload(
            request.content.as_deref(),
            request.content_base64.as_deref(),
        ) {
            Ok(value) => value,
            Err(message) => return ToolEnvelope::error(message, None),
        };
        let path = match resolve_host_file_path(
            &request.path,
            request.working_dir.as_deref(),
            &self.config.host_default_workdir,
        ) {
            Ok(path) => path,
            Err(message) => return ToolEnvelope::error(message, None),
        };
        match self
            .files
            .write_large(
                &path,
                &content_base64,
                request.append,
                request.create_dirs,
                request.expected_sha256.as_deref(),
            )
            .await
        {
            Ok(data) => {
                let summary = if request.append {
                    format!(
                        "Appended large payload to {} on the Windows host and verified the exact bytes.",
                        request.path
                    )
                } else {
                    format!(
                        "Wrote large payload to {} on the Windows host and verified the exact bytes.",
                        request.path
                    )
                };
                let text = large_write_text(&summary, &data);
                ToolEnvelope::success_with_text(summary, serde_json::to_value(data).ok(), text)
            }
            Err(error) => ToolEnvelope::error(error.to_string(), None),
        }
    }

    #[tool(
        name = "host_capture_display",
        description = "Capture the complete host desktop using the native compositor/screenshot backend and return an MCP image content block. Windows returns JPEG; macOS/Linux use lossless PNG when their native tools do.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn host_capture_display(
        &self,
        Parameters(request): Parameters<CaptureDisplayRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.capture_display_internal(request, cancellation).await
    }

    #[tool(
        name = "host_capture_window",
        description = "Capture the largest visible window owned by a host PID or one of its child processes. The Windows backend detects black PrintWindow frames from GPU/DirectComposition surfaces and falls back to compositor-visible pixels.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn host_capture_window(
        &self,
        Parameters(request): Parameters<CaptureWindowRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.capture_window_internal(request, cancellation).await
    }

    #[tool(
        name = "host_capture_program",
        description = "Compatibility alias for host_capture_window.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn host_capture_program(
        &self,
        Parameters(request): Parameters<CaptureWindowRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.capture_window_internal(request, cancellation).await
    }

    #[tool(
        name = "windows_host_capture_display",
        description = "Compatibility alias for host_capture_display. On Windows this retains the original PR tool name.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_capture_display(
        &self,
        Parameters(request): Parameters<CaptureDisplayRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.capture_display_internal(request, cancellation).await
    }

    #[tool(
        name = "windows_host_capture_program",
        description = "Compatibility alias for host_capture_window. On Windows this retains the original PR tool name.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_capture_program(
        &self,
        Parameters(request): Parameters<CaptureWindowRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.capture_window_internal(request, cancellation).await
    }

    #[tool(
        name = "host_exec",
        description = "Run a native host shell command. Prefer host_run_program when shell parsing is unnecessary.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn host_exec(
        &self,
        Parameters(request): Parameters<HostShellToolRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.run_host_shell_internal(request, cancellation).await
    }

    #[tool(
        name = "windows_host_exec",
        description = "Compatibility alias for host_exec.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_exec(
        &self,
        Parameters(request): Parameters<HostShellToolRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.run_host_shell_internal(request, cancellation).await
    }

    #[tool(
        name = "host_run_program",
        description = "Run one HOST_PROGRAM_ALLOWLIST executable directly on the native host without shell parsing.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn host_run_program(
        &self,
        Parameters(request): Parameters<HostProgramToolRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.run_host_program_internal(request, cancellation).await
    }

    #[tool(
        name = "windows_host_run_program",
        description = "Compatibility alias for host_run_program.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_run_program(
        &self,
        Parameters(request): Parameters<HostProgramToolRequest>,
        cancellation: CancellationToken,
    ) -> CallToolResult {
        self.run_host_program_internal(request, cancellation).await
    }

    #[tool(
        name = "host_status",
        description = "Inspect whether native host execution is enabled and the current host/runtime configuration.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn host_status(&self) -> CallToolResult {
        self.host_status_result()
    }

    #[tool(
        name = "windows_host_status",
        description = "Compatibility alias for host_status.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn windows_host_status(&self) -> CallToolResult {
        self.host_status_result()
    }

    fn host_status_result(&self) -> CallToolResult {
        ToolEnvelope::success(
            format!(
                "Fetched {} tool status.",
                host_title(&self.config).to_lowercase()
            ),
            Some(json!({
                "enabled": self.config.host_exec_enabled,
                "platform": self.config.platform.id,
                "platformDisplayName": self.config.platform.display_name,
                "shell": self.config.host_shell,
                "defaultWorkdir": self.config.host_default_workdir,
                "allowlist": self.config.host_program_allowlist,
                "resolvedNodeExe": resolved_program_path(&self.config.node_exe, self.config.platform.is_windows),
                "powerShellExe": self.config.power_shell_exe,
                "powerShellFallbackExe": self.config.power_shell_fallback_exe,
                "powerShellFallbackEnabled": !self.config.power_shell_fallback_exe.is_empty()
                    && self.config.power_shell_fallback_exe != self.config.power_shell_exe,
                "windowsHostExecDefaultsToAdmin": self.config.platform.is_windows,
                "allowWindowsHostExecUac": self.config.allow_windows_host_exec_uac,
            })),
        )
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for DevboxMcp {
    fn get_info(&self) -> ServerInfo {
        let mut capabilities = ServerCapabilities::builder().enable_tools().build();
        capabilities.logging = Some(serde_json::Map::default());
        ServerInfo::new(capabilities).with_server_info(
            Implementation::new(self.config.server_name(), env!("CARGO_PKG_VERSION"))
                .with_website_url("https://github.com/adybag14-cyber/devbox"),
        )
    }

    async fn list_tools(
        &self,
        _request: Option<rmcp::model::PaginatedRequestParams>,
        context: RequestContext<RoleServer>,
    ) -> Result<rmcp::model::ListToolsResult, rmcp::ErrorData> {
        let supports_cache_hints = context
            .protocol_version()
            .is_some_and(|version| version >= rmcp::model::ProtocolVersion::V_2026_07_28);
        Ok(rmcp::model::ListToolsResult {
            result_type: Some(rmcp::model::ResultType::COMPLETE),
            tools: self
                .tool_router
                .list_all()
                .into_iter()
                .map(|tool| self.configured_tool(tool))
                .collect(),
            meta: None,
            next_cursor: None,
            ttl_ms: supports_cache_hints.then_some(0),
            cache_scope: supports_cache_hints.then_some(rmcp::model::CacheScope::Public),
        })
    }

    fn get_tool(&self, name: &str) -> Option<rmcp::model::Tool> {
        self.tool_router
            .get(name)
            .cloned()
            .map(|tool| self.configured_tool(tool))
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        context: RequestContext<RoleServer>,
    ) -> Result<CallToolResponse, rmcp::ErrorData> {
        let invocation =
            ToolUsageInvocation::new(request.name.as_ref(), request.arguments.as_ref(), &context);
        let logger = self.usage.tool_logger();
        logger.append(&invocation.start_event()).await.ok();
        let mut usage_guard = ToolUsageDropGuard::new(logger.clone(), invocation.clone());
        let _active_request = self.active_requests.register_context(&context);
        let result = self
            .tool_router
            .call(ToolCallContext::new(self, request, context))
            .await;
        match &result {
            Ok(response) => logger.append(&invocation.finish_event(response)).await.ok(),
            Err(error) => logger
                .append(&invocation.throw_event(&error.to_string()))
                .await
                .ok(),
        };
        usage_guard.complete();
        result
    }
}

#[derive(Debug, Clone)]
struct HttpState {
    config: Arc<Config>,
    oauth: Option<Arc<OAuthService>>,
    handler: DevboxMcp,
    gateway: Arc<GatewayState>,
}

fn spawn_job_maintenance(handler: &DevboxMcp, config: &Config, cancellation: CancellationToken) {
    let maintenance_store = handler.jobs.store().clone();
    let maintenance_state_path = config.project_root.join("run").join("job-maintenance.json");
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(60));
        loop {
            tokio::select! {
                () = cancellation.cancelled() => return,
                _ = interval.tick() => {
                    let started = Instant::now();
                    match maintenance_store.reconcile_maintenance_batch(100).await {
                        Ok(summary) => {
                            let duration_ms = u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX);
                            tracing::debug!(
                                discovered = summary.discovered,
                                scanned = summary.scanned,
                                batch_limited = summary.batch_limited,
                                interrupted = summary.interrupted,
                                terminal = summary.terminal,
                                deleted = summary.deleted,
                                errors = summary.errors,
                                duration_ms,
                                "Rust MCP job maintenance pass completed"
                            );
                            let _ = write_json_snapshot(
                                &maintenance_state_path,
                                &json!({
                                    "sampledAtUtc": chrono::Utc::now().to_rfc3339(),
                                    "durationMs": duration_ms,
                                    "summary": summary,
                                }),
                            ).await;
                        },
                        Err(error) => {
                            tracing::warn!(%error, "Rust MCP job maintenance pass failed");
                            let _ = write_json_snapshot(
                                &maintenance_state_path,
                                &json!({
                                    "sampledAtUtc": chrono::Utc::now().to_rfc3339(),
                                    "durationMs": u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
                                    "error": error.to_string(),
                                }),
                            ).await;
                        },
                    }
                }
            }
        }
    });
}

pub fn build_router(config: Arc<Config>, cancellation: CancellationToken) -> Router {
    let handler = DevboxMcp::new(config.clone());
    let warm_runtime = handler.runtime.clone();
    let warm_cancellation = cancellation.child_token();
    tokio::spawn(async move {
        let _ = warm_runtime.get_versions(false, warm_cancellation).await;
    });
    let warm_host_runtime = handler.runtime.clone();
    let warm_host_cancellation = cancellation.child_token();
    tokio::spawn(async move {
        if let Err(error) = warm_host_runtime
            .warm_host_execution_state(warm_host_cancellation)
            .await
        {
            tracing::warn!(%error, "Failed to warm Rust host execution state");
        }
    });
    spawn_job_maintenance(&handler, &config, cancellation.child_token());
    let service_handler = handler.clone();
    let transport_config = StreamableHttpServerConfig::default()
        .with_allowed_hosts(transport_allowed_hosts(&config))
        .with_legacy_session_mode(false)
        .with_json_response(false)
        .with_cancellation_token(cancellation);
    let mcp: StreamableHttpService<DevboxMcp, LocalSessionManager> = StreamableHttpService::new(
        move || Ok(service_handler.clone()),
        Arc::default(),
        transport_config,
    );

    let oauth = OAuthService::new(&config).map(Arc::new);
    let active_requests = handler.active_requests.clone();
    let body_limit = config.mcp_json_body_limit_bytes;
    let gateway = Arc::new(GatewayState::new(
        config.clone(),
        handler.usage.http_logger(),
        active_requests.clone(),
    ));
    let state = HttpState {
        config,
        oauth: oauth.clone(),
        handler: handler.clone(),
        gateway: gateway.clone(),
    };
    let mut router = Router::new()
        .route(
            "/",
            get(root_metadata)
                .post_service(mcp.clone())
                .delete(mcp_delete),
        )
        .route("/healthz", get(healthz))
        .route(
            "/mcp",
            get(mcp_sse_probe).post_service(mcp).delete(mcp_delete),
        )
        .with_state(state);
    if let Some(service) = oauth.clone() {
        router = router.merge(crate::oauth::router(service));
    }
    router
        .layer(middleware::from_fn_with_state(
            active_requests,
            crate::request_control::apply_cancellation_notification,
        ))
        .layer(middleware::from_fn_with_state(
            oauth,
            crate::oauth::mcp_bearer_guard,
        ))
        .layer(middleware::from_fn_with_state(
            gateway,
            crate::gateway::guard_and_bridge,
        ))
        .layer(middleware::from_fn_with_state(
            body_limit,
            crate::gateway::json_body_limit,
        ))
        .layer(TraceLayer::new_for_http())
}

/// Start the Rust MCP HTTP server and return the bound socket address.
///
/// # Errors
/// Returns an error when the configured listener cannot be bound or inspected.
pub async fn serve(
    config: Arc<Config>,
    cancellation: CancellationToken,
) -> Result<(SocketAddr, tokio::task::JoinHandle<std::io::Result<()>>)> {
    let address = format!("{}:{}", config.host, config.port);
    let listener = tokio::net::TcpListener::bind(&address)
        .await
        .with_context(|| format!("bind Rust MCP server to {address}"))?;
    let local = listener
        .local_addr()
        .context("read Rust MCP listener address")?;
    let router = build_router(config, cancellation.clone());
    let task = tokio::spawn(async move {
        axum::serve(
            listener,
            router.into_make_service_with_connect_info::<SocketAddr>(),
        )
        .with_graceful_shutdown(cancellation.cancelled_owned())
        .await
    });
    Ok((local, task))
}

fn resolved_program_path(program: &str, windows: bool) -> String {
    let path = std::path::Path::new(program);
    if path.is_absolute() || path.components().count() > 1 {
        return path.to_string_lossy().into_owned();
    }
    let extensions = if windows {
        std::env::var("PATHEXT")
            .unwrap_or_else(|_| ".COM;.EXE;.BAT;.CMD".to_owned())
            .split(';')
            .filter(|value| !value.trim().is_empty())
            .map(|value| value.trim().to_owned())
            .collect::<Vec<_>>()
    } else {
        vec![String::new()]
    };
    let Some(path_var) = std::env::var_os("PATH") else {
        return program.to_owned();
    };
    for directory in std::env::split_paths(&path_var) {
        for extension in &extensions {
            let candidate = if windows && std::path::Path::new(program).extension().is_none() {
                directory.join(format!("{program}{extension}"))
            } else {
                directory.join(program)
            };
            if candidate.is_file() {
                if windows && program.eq_ignore_ascii_case("node") {
                    return candidate
                        .with_file_name("node.exe")
                        .to_string_lossy()
                        .into_owned();
                }
                return candidate.to_string_lossy().into_owned();
            }
        }
    }
    program.to_owned()
}

async fn write_json_snapshot(path: &std::path::Path, value: &Value) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        tokio::fs::create_dir_all(parent).await?;
    }
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    let bytes = serde_json::to_vec_pretty(value).map_err(std::io::Error::other)?;
    tokio::fs::write(&temporary, bytes).await?;
    if tokio::fs::rename(&temporary, path).await.is_err() {
        tokio::fs::remove_file(path).await.ok();
        tokio::fs::rename(&temporary, path).await?;
    }
    Ok(())
}

async fn read_json_snapshot(path: PathBuf) -> Option<Value> {
    let raw = tokio::fs::read_to_string(path).await.ok()?;
    serde_json::from_str(&raw).ok()
}

async fn read_guardian_status_snapshot(config: &Config) -> Option<Value> {
    let state = read_json_snapshot(
        config
            .project_root
            .join("run")
            .join("guardian")
            .join("state.json"),
    )
    .await?;
    Some(json!({
        "observedAtUtc": state.get("ObservedAtUtc").cloned().unwrap_or(Value::Null),
        "isHealthy": state.get("IsHealthy").cloned().unwrap_or(Value::Null),
        "needsRepair": state.get("NeedsRepair").cloned().unwrap_or(Value::Null),
        "mcpElevated": state.get("McpElevated").cloned().unwrap_or(Value::Null),
        "publicTunnelHealthy": state.get("PublicTunnelHealthy").cloned().unwrap_or(Value::Null),
        "cloudflaredRunning": state.get("CloudflaredRunning").cloned().unwrap_or(Value::Null),
        "cloudflaredMetrics": state.get("CloudflaredMetrics").cloned().unwrap_or(Value::Null),
        "cloudflaredMetricsDelta": state.get("CloudflaredMetricsDelta").cloned().unwrap_or(Value::Null),
        "tunnelTransportHealthy": state.get("TunnelTransportHealthy").cloned().unwrap_or(Value::Null),
        "tunnelTransportDegraded": state.get("TunnelTransportDegraded").cloned().unwrap_or(json!(false)),
        "tunnelTransportReasons": state.get("TunnelTransportReasons").cloned().unwrap_or_else(|| json!([])),
        "readiness": state.get("Readiness").cloned().unwrap_or(Value::Null),
        "reasons": state.get("Reasons").cloned().unwrap_or_else(|| json!([])),
    }))
}

async fn healthz() -> &'static str {
    "ok"
}

async fn root_metadata(
    State(state): State<HttpState>,
    Extension(request_context): Extension<GatewayRequestContext>,
    headers: HeaderMap,
) -> Response {
    if accepts_event_stream(&headers) {
        return mcp_sse_probe(headers).await;
    }

    let is_local = request_context.is_local;
    let local_base = resolve_local_base_url(&headers, is_local);
    let connector_base = state
        .config
        .public_base_url
        .clone()
        .or_else(|| local_base.clone());
    let mcp_url = connector_base
        .as_ref()
        .map(|base| format!("{}/mcp", base.trim_end_matches('/')));
    let notes = match state.config.auth_mode {
        AuthMode::CloudflareAccess => {
            "Cloudflare Access-backed OAuth is enabled for ChatGPT app testing. Protect the /authorize path with a Cloudflare Access application."
                .to_owned()
        }
        AuthMode::DemoOauth => format!(
            "Demo OAuth is enabled for ChatGPT app testing. The {} is the main execution environment; host tools are separate and explicit.",
            state.config.runtime_label()
        ),
        AuthMode::None => format!(
            "No authentication mode is active. The {} is the main execution environment; host tools are separate and explicit.",
            state.config.runtime_label()
        ),
    };
    let mut body = serde_json::Map::from_iter([
        ("name".to_owned(), json!(state.config.server_name())),
        ("version".to_owned(), json!(env!("CARGO_PKG_VERSION"))),
        ("build".to_owned(), crate::provenance::snapshot()),
        (
            "auth_mode".to_owned(),
            json!(state.config.auth_mode.as_str()),
        ),
        (
            "runtime_mode".to_owned(),
            json!(state.config.runtime_mode.as_str()),
        ),
        ("platform".to_owned(), json!(state.config.platform.id)),
        (
            "public_base_url".to_owned(),
            json!(state.config.public_base_url),
        ),
        ("local_base_url".to_owned(), json!(local_base)),
        ("mcp_url".to_owned(), json!(mcp_url)),
        ("root_mcp_url".to_owned(), json!(connector_base)),
        (
            "gateway_bridge".to_owned(),
            state.gateway.bridge_info(is_local),
        ),
        (
            "oauth".to_owned(),
            json!(state.oauth.as_ref().map(|service| service.oauth_info())),
        ),
        ("notes".to_owned(), json!(notes)),
    ]);

    if state.config.auth_mode != AuthMode::None && !is_local {
        return (StatusCode::OK, Json(Value::Object(body))).into_response();
    }

    let devbox = state
        .handler
        .lifecycle
        .status(CancellationToken::new())
        .await
        .unwrap_or_else(|error| {
            json!({
                "exists": false,
                "running": false,
                "status": format!("error: {error}"),
            })
        });
    body.insert(
        "runtime".to_owned(),
        json!({
            "runtimeMode": state.config.runtime_mode.as_str(),
            "platform": state.config.platform.id,
            "hostShell": state.config.host_shell,
            "devboxContainerName": state.config.devbox_container_name,
            "devboxImageName": state.config.devbox_image_name,
            "devboxWorkspacePath": state.config.devbox_workspace_path,
            "hostWorkspacePath": state.config.host_workspace_path,
            "hostExecEnabled": state.config.host_exec_enabled,
        }),
    );
    body.insert("devbox".to_owned(), devbox);
    (StatusCode::OK, Json(Value::Object(body))).into_response()
}

fn resolve_local_base_url(headers: &HeaderMap, is_local: bool) -> Option<String> {
    if !is_local {
        return None;
    }
    let host = headers
        .get(header::HOST)
        .and_then(|value| value.to_str().ok())?
        .trim();
    if host.is_empty() {
        return None;
    }
    let protocol = headers
        .get("x-forwarded-proto")
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.split(',').next())
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or("http");
    Some(
        format!("{protocol}://{host}")
            .trim_end_matches('/')
            .to_owned(),
    )
}

async fn mcp_delete() -> impl IntoResponse {
    (StatusCode::OK, "")
}

async fn mcp_sse_probe(headers: HeaderMap) -> Response {
    if !accepts_event_stream(&headers) {
        return (
            StatusCode::NOT_ACCEPTABLE,
            Json(json!({
                "jsonrpc": "2.0",
                "error": {
                    "code": -32000,
                    "message": "Not Acceptable: Client must accept text/event-stream",
                },
                "id": Value::Null,
            })),
        )
            .into_response();
    }

    Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "text/event-stream")
        .header(header::CACHE_CONTROL, "no-cache, no-transform")
        .header(header::CONNECTION, "keep-alive")
        .header("x-accel-buffering", "no")
        .body(Body::from(": mcp-sse-probe\n\n"))
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

fn accepts_event_stream(headers: &HeaderMap) -> bool {
    headers
        .get(header::ACCEPT)
        .and_then(|value| value.to_str().ok())
        .is_some_and(|value| value.contains("text/event-stream"))
}

fn host_title(config: &Config) -> String {
    if config.platform.is_windows {
        "Windows Host".to_owned()
    } else {
        format!("{} Host", config.platform.display_name)
    }
}

fn host_command_title(config: &Config) -> String {
    if config.platform.is_windows {
        "Windows PowerShell".to_owned()
    } else {
        format!("{} Host Shell", config.platform.display_name)
    }
}

fn render_file_process_result(
    summary: String,
    result: Result<ProcessResult, String>,
    max_chars: usize,
) -> CallToolResult {
    match result {
        Ok(result) => {
            let stdout = shape_process_output(&result.stdout, OutputMode::Tail, max_chars, 0);
            let stderr = shape_process_output(&result.stderr, OutputMode::Tail, max_chars, 0);
            ToolEnvelope::process_success(
                summary,
                Some(json!({
                    "output": {
                        "mode": "tail",
                        "max_chars": max_chars,
                        "max_lines": 0,
                        "stdout_original_chars": stdout.original_chars,
                        "stderr_original_chars": stderr.original_chars,
                    }
                })),
                stdout.text,
                stderr.text,
                result.exit_code,
                stdout.truncated || stderr.truncated,
            )
        }
        Err(error) => ToolEnvelope::process_error(error, None, "", "", None, false),
    }
}

fn render_anyhow_tool_error(error: &anyhow::Error, max_chars: usize) -> CallToolResult {
    if let Some(process) = error.downcast_ref::<crate::process::ProcessError>() {
        return render_command_style_error(
            process.message.clone(),
            None,
            &process.stdout,
            &process.stderr,
            process.exit_code,
            max_chars,
        );
    }
    render_command_style_error(error.to_string(), None, "", "", None, max_chars)
}

fn render_oversized_posix_capture_pid(
    config: &Config,
    pid: u64,
    max_chars: usize,
) -> CallToolResult {
    if config.platform.is_linux && !config.platform.is_termux {
        let session_type = std::env::var("XDG_SESSION_TYPE")
            .unwrap_or_default()
            .trim()
            .to_ascii_lowercase();
        let wayland = std::env::var_os("WAYLAND_DISPLAY").is_some() || session_type == "wayland";
        let x11 = std::env::var_os("DISPLAY").is_some() || session_type == "x11";
        let session = if wayland {
            "wayland"
        } else if x11 {
            "x11"
        } else if session_type.is_empty() {
            "headless"
        } else {
            &session_type
        };
        let summary = if !wayland && !x11 {
            format!(
                "No capturable Linux graphical session was detected (session={session}). Set DISPLAY for X11 or WAYLAND_DISPLAY for Wayland and run Devbox inside the logged-in desktop session."
            )
        } else if wayland && !x11 {
            "No PID-selected window could be discovered on this Wayland compositor. Sway and Hyprland are supported directly; other compositors may intentionally hide window/PID enumeration and require an interactive desktop portal.".to_owned()
        } else {
            format!(
                "No visible X11/XWayland window was found for PID {pid} or its child processes."
            )
        };
        return render_command_style_error(summary, None, "", "", None, max_chars);
    }
    if config.platform.is_macos {
        let script_path = std::env::temp_dir()
            .join(format!("devbox-macos-window-capture-rust-{pid}"))
            .join("devbox-window-query.swift")
            .to_string_lossy()
            .into_owned();
        return render_command_style_error(
            "No on-screen CoreGraphics window matched the requested process tree.".to_owned(),
            Some(json!({
                "file": "/usr/bin/swift",
                "args": [script_path, "window", pid.to_string()],
            })),
            "",
            "No on-screen CoreGraphics window matched the requested process tree.\n",
            Some(3),
            max_chars,
        );
    }
    render_command_style_error(
        format!(
            "Failed to capture {} host window for PID {pid}: pid exceeds the native process ID range.",
            config.platform.display_name
        ),
        None,
        "",
        "",
        None,
        max_chars,
    )
}

fn render_windows_capture_pid_binding_error(pid: u64) -> CallToolResult {
    let summary = format!(
        "\u{1b}[31;1mcapture.ps1: \u{1b}[31;1mCannot process argument transformation on parameter 'TargetPid'. Cannot convert value \"{pid}\" to type \"System.Int32\". Error: \"Value was either too large or too small for an Int32.\"\u{1b}[0m"
    );
    ToolEnvelope::process_error(
        summary.clone(),
        None,
        "",
        format!("{summary}\r\n"),
        Some(1),
        false,
    )
}

fn render_command_style_error(
    summary: String,
    data: Option<Value>,
    stdout: &str,
    stderr: &str,
    exit_code: Option<i32>,
    max_chars: usize,
) -> CallToolResult {
    let (stdout, stdout_truncated) = trim_javascript_text(stdout, max_chars);
    let (stderr, stderr_truncated) = trim_javascript_text(stderr, max_chars);
    ToolEnvelope::process_error(
        summary,
        data,
        stdout,
        stderr,
        exit_code,
        stdout_truncated || stderr_truncated,
    )
}

fn trim_javascript_text(text: &str, max_chars: usize) -> (String, bool) {
    if text.is_empty() {
        return (String::new(), false);
    }
    let units = text.encode_utf16().collect::<Vec<_>>();
    if units.len() <= max_chars {
        return (text.to_owned(), false);
    }
    let suffix = format!("\n... truncated to {max_chars} characters ...");
    let suffix_len = suffix.encode_utf16().count();
    let keep = max_chars.saturating_sub(suffix_len);
    let head = String::from_utf16_lossy(&units[..keep.min(units.len())]);
    (format!("{head}{suffix}"), true)
}

fn render_shell_result(
    summary: String,
    context: &ShellRenderContext,
    result: Result<crate::process::ProcessOutput, RuntimeExecError>,
) -> CallToolResult {
    let mode = OutputMode::parse(&context.output_mode);
    let execution = json!({
        "queue_wait_ms": context.queue_wait_ms,
        "slot": context.slot,
    });
    match result {
        Ok(output) => {
            let stdout =
                shape_process_output(&output.stdout, mode, context.max_chars, context.max_lines);
            let stderr =
                shape_process_output(&output.stderr, mode, context.max_chars, context.max_lines);
            let truncated = stdout.truncated || stderr.truncated;
            ToolEnvelope::process_success(
                summary,
                Some(json!({
                    "execution": execution,
                    "output": {
                        "mode": mode.as_str(),
                        "max_chars": context.max_chars,
                        "max_lines": context.max_lines,
                        "stdout_original_chars": stdout.original_chars,
                        "stderr_original_chars": stderr.original_chars,
                    }
                })),
                stdout.text,
                stderr.text,
                output.exit_code,
                truncated,
            )
        }
        Err(error @ RuntimeExecError::WindowsElevationRequired) => ToolEnvelope::process_error(
            error.to_string(),
            Some(json!({
                "execution": execution,
                "bridge_diagnostics": {
                    "suspected_elevation_gap": true,
                    "windows_host_exec_defaults_to_admin": true,
                    "allow_windows_host_exec_uac": false,
                    "hints": [
                        "Keep MCP started only by elevated Guardian / ChatGptDevboxMcp-ElevatedStart (RunLevel Highest).",
                        "Do not start MCP from a normal (non-admin) terminal if you want silent elevated host_exec.",
                        "Set ALLOW_WINDOWS_HOST_EXEC_UAC=true only if you intentionally want per-command UAC prompts."
                    ]
                }
            })),
            "",
            "",
            Some(740),
            false,
        ),
        Err(RuntimeExecError::Process(error)) => render_command_style_error(
            error.message,
            Some(json!({ "execution": execution })),
            &error.stdout,
            &error.stderr,
            error.exit_code,
            context.error_max_chars,
        ),
        Err(error) => render_command_style_error(
            error.to_string(),
            Some(json!({ "execution": execution })),
            "",
            "",
            None,
            context.error_max_chars,
        ),
    }
}

fn render_program_result(
    summary: String,
    context: &ProgramRenderContext,
    result: Result<crate::process::ProcessOutput, RuntimeExecError>,
) -> CallToolResult {
    let mode = OutputMode::parse(&context.output_mode);
    let execution = json!({
        "queue_wait_ms": context.queue_wait_ms,
        "slot": context.slot,
    });
    match result {
        Ok(output) => {
            let stdout =
                shape_process_output(&output.stdout, mode, context.max_chars, context.max_lines);
            let stderr =
                shape_process_output(&output.stderr, mode, context.max_chars, context.max_lines);
            let truncated = stdout.truncated || stderr.truncated;
            let data = json!({
                "execution": execution,
                "output": {
                    "mode": mode.as_str(),
                    "max_chars": context.max_chars,
                    "max_lines": context.max_lines,
                    "stdout_original_chars": stdout.original_chars,
                    "stderr_original_chars": stderr.original_chars,
                }
            });
            ToolEnvelope::process_success(
                summary,
                Some(data),
                stdout.text,
                stderr.text,
                output.exit_code,
                truncated,
            )
        }
        Err(RuntimeExecError::Process(error)) => render_command_style_error(
            error.message,
            Some(json!({ "execution": execution })),
            &error.stdout,
            &error.stderr,
            error.exit_code,
            context.error_max_chars,
        ),
        Err(error) => render_command_style_error(
            error.to_string(),
            Some(json!({ "execution": execution })),
            "",
            "",
            None,
            context.error_max_chars,
        ),
    }
}

fn resolve_host_file_path(
    requested: &str,
    working_dir: Option<&str>,
    default_working_dir: &std::path::Path,
) -> Result<PathBuf, String> {
    let raw = requested.trim();
    if raw.is_empty() {
        return Err("path must not be empty".to_owned());
    }
    if raw.contains("://") || raw.starts_with('$') {
        return Err(format!(
            "Could not resolve a Windows host path from \"{requested}\"."
        ));
    }
    let base = working_dir
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map_or_else(
            || default_working_dir.to_string_lossy().into_owned(),
            str::to_owned,
        );
    let resolved = if let Some(rest) = raw.strip_prefix('~') {
        let home = std::env::var("USERPROFILE")
            .or_else(|_| std::env::var("HOME"))
            .map_err(|_| "Could not resolve the host home directory.".to_owned())?;
        win32_resolve(&home, rest)
    } else if win32_is_absolute(raw) {
        win32_normalize(raw)
    } else {
        win32_resolve(&base, raw)
    };
    Ok(PathBuf::from(resolved))
}

fn win32_is_absolute(value: &str) -> bool {
    let bytes = value.as_bytes();
    matches!(bytes.first(), Some(b'/' | b'\\'))
        || (bytes.len() >= 3 && bytes[1] == b':' && matches!(bytes[2], b'/' | b'\\'))
}

fn win32_drive(value: &str) -> Option<&str> {
    let bytes = value.as_bytes();
    (bytes.len() >= 2 && bytes[1] == b':' && bytes[0].is_ascii_alphabetic()).then(|| &value[..2])
}

fn win32_normalize(value: &str) -> String {
    let replaced = value.replace('/', "\\");
    let drive = win32_drive(&replaced).map(str::to_owned);
    let tail = drive.as_ref().map_or(replaced.as_str(), |_| &replaced[2..]);
    let rooted = tail.starts_with('\\');
    let unc = drive.is_none() && tail.starts_with("\\\\");
    let mut parts = Vec::<&str>::new();
    for part in tail
        .split('\\')
        .filter(|part| !part.is_empty() && *part != ".")
    {
        if part == ".." {
            if parts.last().is_some_and(|last| *last != "..") {
                parts.pop();
            } else if !rooted {
                parts.push(part);
            }
        } else {
            parts.push(part);
        }
    }
    let mut out = String::new();
    if let Some(drive) = drive {
        out.push_str(&drive);
    }
    if unc {
        out.push_str("\\\\");
    } else if rooted {
        out.push('\\');
    }
    out.push_str(&parts.join("\\"));
    if out.is_empty() {
        ".".to_owned()
    } else if out.len() == 2 && out.as_bytes()[1] == b':' && rooted {
        format!("{out}\\")
    } else {
        out
    }
}

fn win32_resolve(base: &str, value: &str) -> String {
    if win32_is_absolute(value) {
        let normalized = win32_normalize(value);
        if win32_drive(value).is_none()
            && let Some(drive) = win32_drive(base)
        {
            return format!("{drive}{normalized}");
        }
        return normalized;
    }
    let separator = if base.ends_with(['/', '\\']) {
        ""
    } else {
        "\\"
    };
    win32_normalize(&format!("{base}{separator}{value}"))
}

fn normalize_large_write_payload(
    content: Option<&str>,
    content_base64: Option<&str>,
) -> Result<String, String> {
    match (content, content_base64) {
        (Some(_), Some(_)) => Err("Provide either content or content_base64, not both.".to_owned()),
        (None, None) => Err("Either content or content_base64 is required.".to_owned()),
        (Some(text), None) => Ok(STANDARD.encode(text.as_bytes())),
        (None, Some(encoded)) => Ok(encoded.to_owned()),
    }
}

fn large_read_text(summary: &str, data: &LargeReadResult) -> String {
    let metadata = json!({
        "path": data.path,
        "file_size": data.file_size,
        "offset_bytes_requested": data.offset_bytes_requested,
        "offset_bytes": data.offset_bytes,
        "bytes_requested": data.bytes_requested,
        "bytes_returned": data.bytes_returned,
        "next_offset_bytes": data.next_offset_bytes,
        "eof": data.eof,
        "content_sha256": data.content_sha256,
        "content_base64_chars": data.content_base64.len(),
    });
    format!(
        "{summary}\n\n{}",
        serde_json::to_string_pretty(&metadata).unwrap_or_else(|_| metadata.to_string())
    )
}

fn large_write_text(summary: &str, data: &LargeWriteResult) -> String {
    let metadata = json!({
        "path": data.path,
        "append": data.append,
        "previous_file_size": data.previous_file_size,
        "final_file_size": data.final_file_size,
        "bytes_written": data.bytes_written,
        "content_sha256": data.content_sha256,
        "verification_mode": data.verification_mode,
        "verified": data.verified,
        "expected_sha256_verified": data.expected_sha256_verified,
        "target_existed": data.target_existed,
    });
    format!(
        "{summary}\n\n{}",
        serde_json::to_string_pretty(&metadata).unwrap_or_else(|_| metadata.to_string())
    )
}

#[derive(Debug, Clone)]
struct PathState {
    exists: Option<bool>,
    is_file: Option<bool>,
    is_directory: Option<bool>,
    size: Option<u64>,
    mtime_ms: Option<f64>,
    mtime_utc: Option<String>,
    transient_error: Option<String>,
}

impl PathState {
    const fn missing() -> Self {
        Self {
            exists: Some(false),
            is_file: None,
            is_directory: None,
            size: None,
            mtime_ms: None,
            mtime_utc: None,
            transient_error: None,
        }
    }

    fn transient(code: &str) -> Self {
        Self {
            exists: None,
            is_file: None,
            is_directory: None,
            size: None,
            mtime_ms: None,
            mtime_utc: None,
            transient_error: Some(code.to_owned()),
        }
    }
}

async fn wait_for_path(
    request: WaitForFileRequest,
    max_wait_seconds: f64,
    cancellation: CancellationToken,
) -> Result<serde_json::Value, String> {
    if request.path.trim().is_empty() {
        return Err("path must not be empty".to_owned());
    }
    let timeout_seconds = request
        .timeout_seconds
        .unwrap_or(max_wait_seconds.min(60.0));
    if !timeout_seconds.is_finite() || timeout_seconds < 0.1 || timeout_seconds > max_wait_seconds {
        return Err(format!(
            "timeout_seconds must be between 0.1 and {max_wait_seconds}"
        ));
    }
    if !(50..=5000).contains(&request.poll_ms) {
        return Err("poll_ms must be between 50 and 5000".to_owned());
    }
    if request.stable_ms > 30_000 {
        return Err("stable_ms must be at most 30000".to_owned());
    }

    let path = PathBuf::from(&request.path);
    let started = Instant::now();
    let deadline = started + Duration::from_secs_f64(timeout_seconds);
    let mut stable_since = None;

    loop {
        let state = path_state(&path)
            .await
            .map_err(|error| format!("Failed while waiting for {}: {error}", request.path))?;
        let condition = if request.should_exist {
            state.exists == Some(true) && state.size.unwrap_or(0) >= request.min_bytes
        } else {
            state.exists == Some(false)
        };
        let now = Instant::now();
        if condition {
            let since = stable_since.get_or_insert(now);
            if now.duration_since(*since).as_millis() >= u128::from(request.stable_ms) {
                return Ok(path_state_json(
                    &request.path,
                    &state,
                    true,
                    if request.stable_ms > 0 {
                        Some(request.stable_ms)
                    } else {
                        None
                    },
                    started.elapsed(),
                    false,
                ));
            }
        } else {
            stable_since = None;
        }
        if now >= deadline {
            return Ok(path_state_json(
                &request.path,
                &state,
                false,
                None,
                started.elapsed(),
                true,
            ));
        }
        let sleep_for = deadline
            .saturating_duration_since(now)
            .min(Duration::from_millis(request.poll_ms));
        tokio::select! {
            () = tokio::time::sleep(sleep_for) => {},
            () = cancellation.cancelled() => {
                return Err(format!("Wait for {} was cancelled.", request.path));
            }
        }
    }
}

async fn path_state(path: &std::path::Path) -> std::io::Result<PathState> {
    match tokio::fs::metadata(path).await {
        Ok(metadata) => {
            let modified = metadata.modified().ok();
            let mtime_ms = modified
                .and_then(|value| value.duration_since(UNIX_EPOCH).ok())
                .map(|value| {
                    u64_as_javascript_number(value.as_secs()) * 1000.0
                        + f64::from(value.subsec_nanos()) / 1_000_000.0
                });
            let mtime_utc = modified.and_then(format_javascript_timeclip_utc);
            Ok(PathState {
                exists: Some(true),
                is_file: Some(metadata.is_file()),
                is_directory: Some(metadata.is_dir()),
                size: Some(metadata.len()),
                mtime_ms,
                mtime_utc,
                transient_error: None,
            })
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(PathState::missing()),
        Err(error) if error.kind() == std::io::ErrorKind::PermissionDenied => {
            Ok(PathState::transient("EPERM"))
        }
        Err(error) => Err(error),
    }
}

fn u64_as_javascript_number(value: u64) -> f64 {
    let high = u32::try_from(value >> 32).unwrap_or(u32::MAX);
    let low = u32::try_from(value & u64::from(u32::MAX)).unwrap_or(u32::MAX);
    f64::from(high) * 4_294_967_296.0 + f64::from(low)
}

fn format_javascript_timeclip_utc(value: std::time::SystemTime) -> Option<String> {
    let duration = value.duration_since(UNIX_EPOCH).ok()?;
    let millis = duration.as_millis();
    let nanos = i128::try_from(millis).ok()?.saturating_mul(1_000_000);
    let value = time::OffsetDateTime::from_unix_timestamp_nanos(nanos).ok()?;
    Some(format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        value.year(),
        value.month() as u8,
        value.day(),
        value.hour(),
        value.minute(),
        value.second(),
        value.nanosecond() / 1_000_000,
    ))
}

fn path_state_json(
    _path: &str,
    state: &PathState,
    condition_met: bool,
    stable_ms: Option<u64>,
    elapsed: Duration,
    timed_out: bool,
) -> serde_json::Value {
    let mut data = serde_json::Map::new();
    data.insert("exists".to_owned(), json!(state.exists));
    if let Some(value) = state.is_file {
        data.insert("isFile".to_owned(), json!(value));
    }
    if let Some(value) = state.is_directory {
        data.insert("isDirectory".to_owned(), json!(value));
    }
    if let Some(value) = state.size {
        data.insert("size".to_owned(), json!(value));
    }
    if let Some(value) = state.mtime_ms {
        data.insert("mtimeMs".to_owned(), json!(value));
    }
    if let Some(value) = state.mtime_utc.as_ref() {
        data.insert("mtimeUtc".to_owned(), json!(value));
    }
    if let Some(value) = state.transient_error.as_ref() {
        data.insert("transientError".to_owned(), json!(value));
    }
    data.insert("conditionMet".to_owned(), json!(condition_met));
    if let Some(value) = stable_ms {
        data.insert("stableMs".to_owned(), json!(value));
    }
    data.insert(
        "waitedMs".to_owned(),
        json!(u64::try_from(elapsed.as_millis()).unwrap_or(u64::MAX)),
    );
    data.insert("timedOut".to_owned(), json!(timed_out));
    serde_json::Value::Object(data)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn host_large_file_tools_keep_base64_out_of_text_content() {
        let temp = tempfile::tempdir().expect("temp dir");
        let config = Arc::new(Config {
            project_root: temp.path().to_path_buf(),
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
            oauth_state_file_path: temp.path().join("oauth-state.json"),
            cloudflare_access_team_domain: None,
            cloudflare_access_aud: String::new(),
            cloudflare_access_jwks_url: None,
            host_workspace_path: temp.path().to_path_buf(),
            devbox_workspace_path: temp.path().to_path_buf(),
            devbox_container_name: "chatgpt-devbox-runtime".to_owned(),
            devbox_image_name: "chatgpt-devbox-runtime:local".to_owned(),
            devbox_tmp_volume_name: "chatgpt-devbox-runtime-tmp".to_owned(),
            devbox_retired_container_grace_ms: 300_000,
            devbox_auto_start: true,
            devbox_version_cache_ms: 120_000,
            docker_command_timeout_ms: 120_000,
            devbox_default_user: "root".to_owned(),
            host_default_workdir: temp.path().to_path_buf(),
            host_shell: "unused".to_owned(),
            power_shell_exe: "pwsh".to_owned(),
            power_shell_fallback_exe: "powershell.exe".to_owned(),
            node_exe: "node".to_owned(),
            host_program_allowlist: vec!["node".to_owned()],
            host_search_backend: crate::config::HostSearchBackend::Auto,
            devbox_program_allowlist: vec!["node".to_owned()],
            host_exec_enabled: true,
            allow_windows_host_exec_uac: false,
            execution_slot_root: temp.path().join("execution-slots"),
            jobs_root: temp.path().join("jobs"),
            mcp_performance_state_path: temp.path().join("mcp-performance.json"),
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
        });
        let server = DevboxMcp::new(config);
        let write = server
            .windows_host_write_large_file(Parameters(HostLargeWriteRequest {
                path: "fixture.bin".to_owned(),
                working_dir: None,
                content: Some("alpha".to_owned()),
                content_base64: None,
                append: false,
                create_dirs: true,
                expected_sha256: None,
            }))
            .await;
        assert!(!write.is_error.unwrap_or(false));

        let read = server
            .windows_host_read_large_file(Parameters(HostLargeReadRequest {
                path: "fixture.bin".to_owned(),
                working_dir: None,
                offset_bytes: 0,
                max_bytes: 262_144,
            }))
            .await;
        assert!(!read.is_error.unwrap_or(false));
        let structured = read.structured_content.expect("structured content");
        assert_eq!(structured["data"]["content_base64"], "YWxwaGE=");
        let text = read.content[0]
            .as_text()
            .expect("text content")
            .text
            .clone();
        assert!(!text.contains("YWxwaGE="));
        assert!(text.contains("content_base64_chars"));
    }

    #[test]
    fn legacy_windows_host_paths_use_win32_semantics_on_every_platform() {
        assert_eq!(win32_normalize("/tmp/alpha/../beta.txt"), "\\tmp\\beta.txt");
        assert_eq!(
            win32_resolve("/tmp/base", "nested/file.bin"),
            "\\tmp\\base\\nested\\file.bin"
        );
        assert_eq!(win32_normalize("C:/work/./file.txt"), "C:\\work\\file.txt");
        assert!(win32_is_absolute("/tmp/file.txt"));
        assert!(win32_is_absolute("C:/work/file.txt"));
        assert!(!win32_is_absolute("relative/file.txt"));
    }

    #[test]
    fn windows_elevation_refusal_renders_exit_740_and_bridge_diagnostics() {
        let context = ShellRenderContext {
            output_mode: "tail".to_owned(),
            max_chars: 2_000,
            max_lines: 0,
            error_max_chars: 65_536,
            queue_wait_ms: 7,
            slot: Some(2),
        };
        let result = render_shell_result(
            "Windows host".to_owned(),
            &context,
            Err(RuntimeExecError::WindowsElevationRequired),
        );
        assert!(result.is_error.unwrap_or(false));
        let structured = result.structured_content.expect("structured content");
        assert_eq!(structured["exitCode"], 740);
        assert_eq!(
            structured["data"]["bridge_diagnostics"]["suspected_elevation_gap"],
            true
        );
        assert_eq!(
            structured["data"]["bridge_diagnostics"]["windows_host_exec_defaults_to_admin"],
            true
        );
        assert_eq!(
            structured["data"]["bridge_diagnostics"]["allow_windows_host_exec_uac"],
            false
        );
        assert_eq!(structured["data"]["execution"]["queue_wait_ms"], 7);
        assert_eq!(
            structured["data"]["bridge_diagnostics"]["hints"]
                .as_array()
                .map(Vec::len),
            Some(3)
        );
        assert!(
            structured["summary"]
                .as_str()
                .is_some_and(|value| value.contains("medium-integrity"))
        );
    }

    #[test]
    fn command_error_trimming_uses_javascript_utf16_units_and_marker() {
        let input = format!("{}END", "😀".repeat(80));
        let (trimmed, truncated) = trim_javascript_text(&input, 100);
        assert!(truncated);
        assert!(trimmed.encode_utf16().count() <= 100);
        assert!(trimmed.contains("... truncated to 100 characters ..."));
        assert!(!trimmed.ends_with("END"));
    }

    #[tokio::test]
    async fn file_wait_observes_creation_without_a_subprocess() {
        let temp = tempfile::tempdir().expect("temp dir");
        let path = temp.path().join("ready.txt");
        let writer = path.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(40)).await;
            tokio::fs::write(writer, b"ready")
                .await
                .expect("write fixture");
        });
        let value = wait_for_path(
            WaitForFileRequest {
                path: path.to_string_lossy().into_owned(),
                should_exist: true,
                min_bytes: 5,
                stable_ms: 0,
                timeout_seconds: Some(1.0),
                poll_ms: 50,
            },
            5.0,
            CancellationToken::new(),
        )
        .await
        .expect("wait succeeds");
        assert_eq!(value["conditionMet"], true);
        assert_eq!(value["size"], 5);
    }
}
