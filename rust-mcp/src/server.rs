use std::{
    net::SocketAddr,
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant, UNIX_EPOCH},
};

use anyhow::{Context, Result};
use axum::{
    Json, Router,
    extract::State,
    http::{HeaderMap, StatusCode},
    response::IntoResponse,
    routing::get,
};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use rmcp::{
    ServerHandler,
    handler::server::router::tool::ToolRouter,
    handler::server::wrapper::Parameters,
    model::{CallToolResult, Implementation, ServerCapabilities, ServerInfo},
    schemars, tool, tool_handler, tool_router,
    transport::streamable_http_server::{
        StreamableHttpServerConfig, StreamableHttpService, session::local::LocalSessionManager,
    },
};
use serde::Deserialize;
use serde_json::json;
use tokio_util::sync::CancellationToken;
use tower_http::{cors::CorsLayer, trace::TraceLayer};

use crate::{
    Config, RuntimeMode,
    contract::ParityReport,
    execution::{AcquireRequest, ExecutionScheduler, SchedulerConfig},
    files::{FileService, LargeReadResult, LargeWriteResult},
    job_manager::{JobManager, StartProgramJob},
    output::{OutputMode, shape_process_output},
    result::ToolEnvelope,
    runtime::{ProgramRequest, RuntimeExecError, RuntimeExecutor},
};

#[derive(Debug, Clone)]
pub struct DevboxMcp {
    config: Arc<Config>,
    files: Arc<FileService>,
    scheduler: Arc<ExecutionScheduler>,
    runtime: Arc<RuntimeExecutor>,
    jobs: Arc<JobManager>,
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
        Self {
            runtime: Arc::new(RuntimeExecutor::new(config.clone())),
            jobs: Arc::new(JobManager::new(config.clone())),
            config,
            files: Arc::new(FileService::new()),
            scheduler,
            tool_router: Self::tool_router(),
        }
    }

    #[must_use]
    pub fn config(&self) -> &Config {
        &self.config
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
    #[serde(default = "default_wait_timeout")]
    #[schemars(description = "Maximum wait duration in seconds.")]
    timeout_seconds: f64,
    #[serde(default = "default_poll_ms")]
    #[schemars(description = "Filesystem poll interval in milliseconds.")]
    poll_ms: u64,
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
    #[schemars(description = "Optional UTF-8 stdin payload.")]
    input: Option<String>,
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
    #[serde(default = "default_output_chars")]
    #[schemars(description = "Maximum characters returned per output stream.")]
    max_output_chars: usize,
    #[serde(default)]
    #[schemars(
        description = "Optional maximum lines returned per stream; 0 disables line limiting."
    )]
    max_output_lines: usize,
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

struct ProgramRenderContext {
    program: String,
    output_mode: String,
    max_chars: usize,
    max_lines: usize,
    queue_wait_ms: u64,
    slot: Option<usize>,
    slots: Vec<usize>,
    pool: String,
    weight: usize,
}

const fn default_sync_timeout() -> u64 {
    90
}
const fn default_async_timeout() -> u64 {
    7_200
}
const fn default_output_chars() -> usize {
    65_536
}
const fn default_job_log_chars() -> usize {
    20_000
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
const fn default_wait_timeout() -> f64 {
    60.0
}
const fn default_poll_ms() -> u64 {
    250
}
const fn default_large_read_bytes() -> usize {
    262_144
}

#[tool_router(router = tool_router)]
impl DevboxMcp {
    #[tool(
        name = "devbox_status",
        description = "Use this when you need the current state of the selected Devbox runtime.",
        output_schema = rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>()
    )]
    async fn devbox_status(&self) -> CallToolResult {
        ToolEnvelope::success(
            format!(
                "Fetched {} status from the Rust MCP replacement.",
                self.config.runtime_label()
            ),
            Some(json!({
                "mode": self.config.runtime_mode.as_str(),
                "exists": true,
                "running": true,
                "status": "ready",
                "name": if self.config.runtime_mode == RuntimeMode::Host { "host-runtime" } else { "docker-runtime" },
                "workspacePath": self.config.host_workspace_path,
                "platform": self.config.platform.id,
                "hostDefaultWorkdir": self.config.host_default_workdir,
                "hostShell": self.config.host_shell,
                "hostWorkspacePath": self.config.host_workspace_path,
                "devboxWorkspacePath": self.config.devbox_workspace_path,
                "hostExecEnabled": self.config.host_exec_enabled,
                "rustReplacement": ParityReport::current(),
            })),
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
        match wait_for_path(
            request,
            self.config.max_wait_seconds.min(85.0),
            cancellation,
        )
        .await
        {
            Ok(data) => {
                let path = data["path"].as_str().unwrap_or("file");
                let summary = if data["conditionMet"].as_bool().unwrap_or(false) {
                    format!("File condition satisfied for {path}.")
                } else {
                    format!("Timed out waiting for file condition at {path}.")
                };
                ToolEnvelope::success(summary, Some(data))
            }
            Err(message) => ToolEnvelope::error(message, None),
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
        if request.max_output_chars < 100
            || request.max_output_chars > self.config.max_mcp_transfer_chars
        {
            return ToolEnvelope::error(
                format!(
                    "max_output_chars must be between 100 and {}",
                    self.config.max_mcp_transfer_chars
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
                    input: request.input.map(String::into_bytes),
                    working_dir,
                    timeout: Duration::from_secs(request.timeout_seconds),
                    user: request.user,
                    max_capture_chars: Some(self.config.max_mcp_transfer_chars),
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
            &self.config.runtime_label(),
            &ProgramRenderContext {
                program: request.program,
                output_mode: request.output_mode,
                max_chars: request.max_output_chars,
                max_lines: request.max_output_lines,
                queue_wait_ms: lease.queue_wait_ms,
                slot: lease.slot,
                slots: lease.slots.clone(),
                pool: lease.pool.clone(),
                weight: lease.weight,
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
                    "Started {} as detached job {} in {}.",
                    request.program.trim(),
                    job.id,
                    self.config.runtime_label()
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
                    Duration::from_millis(250),
                    &cancellation,
                )
                .await
        };
        match result {
            Ok(status) => ToolEnvelope::success(
                format!("Fetched status for detached job {}.", request.job_id),
                Some(status),
            ),
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
                format!("Fetched logs for detached job {}.", request.job_id),
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
            Ok(status) => ToolEnvelope::success(
                format!(
                    "Cancellation requested for detached job {}.",
                    request.job_id
                ),
                Some(status),
            ),
            Err(error) => ToolEnvelope::error(
                format!("Failed to cancel detached job {}: {error}", request.job_id),
                None,
            ),
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
            Err(error) => ToolEnvelope::error(
                format!(
                    "Failed to read {} from byte {} on the Windows host: {error}",
                    request.path, request.offset_bytes
                ),
                None,
            ),
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
            Err(error) => ToolEnvelope::error(
                format!(
                    "Failed to write large payload to {} on the Windows host: {error}",
                    request.path
                ),
                None,
            ),
        }
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
                "Fetched {} host tool status.",
                self.config.platform.display_name
            ),
            Some(json!({
                "enabled": self.config.host_exec_enabled,
                "platform": self.config.platform.id,
                "platformDisplayName": self.config.platform.display_name,
                "shell": self.config.host_shell,
                "defaultWorkdir": self.config.host_default_workdir,
                "rustReplacement": true,
            })),
        )
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for DevboxMcp {
    fn get_info(&self) -> ServerInfo {
        ServerInfo::new(ServerCapabilities::builder().enable_tools().build())
            .with_server_info(Implementation::new(
                self.config.server_name(),
                env!("CARGO_PKG_VERSION"),
            ))
            .with_instructions(
                "Native Rust replacement for the Devbox MCP. Draft migration: only tools listed by this server are implemented; missing parity is reported at GET /.",
            )
    }
}

#[derive(Debug, Clone)]
struct HttpState {
    config: Arc<Config>,
}

pub fn build_router(config: Arc<Config>, cancellation: CancellationToken) -> Router {
    let service_config = config.clone();
    let transport_config = StreamableHttpServerConfig::default()
        .with_legacy_session_mode(false)
        .with_json_response(false)
        .with_cancellation_token(cancellation);
    let mcp: StreamableHttpService<DevboxMcp, LocalSessionManager> = StreamableHttpService::new(
        move || Ok(DevboxMcp::new(service_config.clone())),
        Arc::default(),
        transport_config,
    );

    Router::new()
        .route("/", get(root_metadata).post_service(mcp.clone()))
        .route("/healthz", get(healthz))
        .nest_service("/mcp", mcp)
        .with_state(HttpState { config })
        .layer(CorsLayer::permissive())
        .layer(TraceLayer::new_for_http())
}

/// Start the Rust MCP HTTP server and return the bound socket address.
///
/// # Errors
/// Returns an error when the configured listener cannot be bound or inspected.
pub async fn serve(config: Arc<Config>, cancellation: CancellationToken) -> Result<SocketAddr> {
    let address = format!("{}:{}", config.host, config.port);
    let listener = tokio::net::TcpListener::bind(&address)
        .await
        .with_context(|| format!("bind Rust MCP server to {address}"))?;
    let local = listener
        .local_addr()
        .context("read Rust MCP listener address")?;
    let router = build_router(config, cancellation.clone());
    tokio::spawn(async move {
        if let Err(error) = axum::serve(listener, router)
            .with_graceful_shutdown(cancellation.cancelled_owned())
            .await
        {
            tracing::error!(%error, "Rust MCP HTTP server stopped with an error");
        }
    });
    Ok(local)
}

async fn healthz() -> &'static str {
    "ok"
}

async fn root_metadata(State(state): State<HttpState>, headers: HeaderMap) -> impl IntoResponse {
    let host = headers
        .get("host")
        .and_then(|value| value.to_str().ok())
        .unwrap_or("localhost");
    let scheme = headers
        .get("x-forwarded-proto")
        .and_then(|value| value.to_str().ok())
        .unwrap_or("http");
    let local_base = format!("{scheme}://{host}");
    let connector_base = state
        .config
        .public_base_url
        .clone()
        .unwrap_or_else(|| local_base.clone());
    let parity = ParityReport::current();
    let body = json!({
        "name": state.config.server_name(),
        "version": env!("CARGO_PKG_VERSION"),
        "implementation": "rust",
        "auth_mode": state.config.auth_mode.as_str(),
        "runtime_mode": state.config.runtime_mode.as_str(),
        "platform": state.config.platform.id,
        "public_base_url": state.config.public_base_url,
        "local_base_url": local_base,
        "mcp_url": format!("{connector_base}/mcp"),
        "root_mcp_url": connector_base,
        "rust_replacement": {
            "draft": true,
            "parity": parity,
        },
    });
    (StatusCode::OK, Json(body))
}

fn render_program_result(
    runtime_label: &str,
    context: &ProgramRenderContext,
    result: Result<crate::process::ProcessOutput, RuntimeExecError>,
) -> CallToolResult {
    let mode = OutputMode::parse(&context.output_mode);
    let execution = json!({
        "queue_wait_ms": context.queue_wait_ms,
        "slot": context.slot,
        "slots": context.slots,
        "pool": context.pool,
        "weight": context.weight,
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
                format!("Ran {} directly in {runtime_label}.", context.program),
                Some(data),
                stdout.text,
                stderr.text,
                output.exit_code,
                truncated,
            )
        }
        Err(RuntimeExecError::Process(error)) => {
            let stdout =
                shape_process_output(&error.stdout, mode, context.max_chars, context.max_lines);
            let stderr =
                shape_process_output(&error.stderr, mode, context.max_chars, context.max_lines);
            let truncated = stdout.truncated || stderr.truncated;
            let data = json!({
                "execution": execution,
                "output": {
                    "mode": mode.as_str(),
                    "max_chars": context.max_chars,
                    "max_lines": context.max_lines,
                    "stdout_original_chars": stdout.original_chars,
                    "stderr_original_chars": stderr.original_chars,
                },
                "timed_out": error.timed_out,
                "aborted": error.aborted,
            });
            ToolEnvelope::process_error(
                error.message,
                Some(data),
                stdout.text,
                stderr.text,
                error.exit_code,
                truncated,
            )
        }
        Err(error) => {
            ToolEnvelope::error(error.to_string(), Some(json!({ "execution": execution })))
        }
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
        .map_or_else(|| default_working_dir.to_path_buf(), PathBuf::from);
    let path = if let Some(rest) = raw.strip_prefix('~') {
        let home = std::env::var_os("USERPROFILE")
            .or_else(|| std::env::var_os("HOME"))
            .map(PathBuf::from)
            .ok_or_else(|| "Could not resolve the host home directory.".to_owned())?;
        home.join(rest.trim_start_matches(['/', '\\']))
    } else {
        PathBuf::from(raw)
    };
    Ok(if path.is_absolute() {
        path
    } else {
        base.join(path)
    })
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
    exists: bool,
    is_file: bool,
    is_directory: bool,
    size: u64,
    mtime_ms: Option<u128>,
}

impl PathState {
    const fn missing() -> Self {
        Self {
            exists: false,
            is_file: false,
            is_directory: false,
            size: 0,
            mtime_ms: None,
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
    if !request.timeout_seconds.is_finite()
        || request.timeout_seconds < 0.1
        || request.timeout_seconds > max_wait_seconds
    {
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
    let deadline = started + Duration::from_secs_f64(request.timeout_seconds);
    let mut stable_since = None;

    loop {
        let state = path_state(&path)
            .await
            .map_err(|error| format!("Failed while waiting for {}: {error}", request.path))?;
        let condition = if request.should_exist {
            state.exists && (!state.is_file || state.size >= request.min_bytes)
        } else {
            !state.exists
        };
        let now = Instant::now();
        if condition {
            let since = stable_since.get_or_insert(now);
            if now.duration_since(*since).as_millis() >= u128::from(request.stable_ms) {
                return Ok(path_state_json(
                    &request.path,
                    &state,
                    true,
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
        Ok(metadata) => Ok(PathState {
            exists: true,
            is_file: metadata.is_file(),
            is_directory: metadata.is_dir(),
            size: metadata.len(),
            mtime_ms: metadata
                .modified()
                .ok()
                .and_then(|value| value.duration_since(UNIX_EPOCH).ok())
                .map(|value| value.as_millis()),
        }),
        Err(error)
            if matches!(
                error.kind(),
                std::io::ErrorKind::NotFound | std::io::ErrorKind::PermissionDenied
            ) =>
        {
            Ok(PathState::missing())
        }
        Err(error) => Err(error),
    }
}

fn path_state_json(
    path: &str,
    state: &PathState,
    condition_met: bool,
    elapsed: Duration,
    timed_out: bool,
) -> serde_json::Value {
    json!({
        "path": path,
        "exists": state.exists,
        "isFile": state.is_file,
        "isDirectory": state.is_directory,
        "size": state.size,
        "mtimeMs": state.mtime_ms,
        "conditionMet": condition_met,
        "waitedMs": u64::try_from(elapsed.as_millis()).unwrap_or(u64::MAX),
        "timedOut": timed_out,
    })
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
            host_workspace_path: temp.path().to_path_buf(),
            devbox_workspace_path: temp.path().to_path_buf(),
            devbox_container_name: "chatgpt-devbox-runtime".to_owned(),
            devbox_default_user: "root".to_owned(),
            host_default_workdir: temp.path().to_path_buf(),
            host_shell: "unused".to_owned(),
            node_exe: "node".to_owned(),
            host_program_allowlist: vec!["node".to_owned()],
            devbox_program_allowlist: vec!["node".to_owned()],
            host_exec_enabled: true,
            execution_slot_root: temp.path().join("execution-slots"),
            jobs_root: temp.path().join("jobs"),
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
            max_wait_seconds: 85.0,
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
                timeout_seconds: 1.0,
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
