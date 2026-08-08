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

use crate::{Config, RuntimeMode, contract::ParityReport, result::ToolEnvelope};

#[derive(Debug, Clone)]
pub struct DevboxMcp {
    config: Arc<Config>,
    tool_router: ToolRouter<Self>,
}

impl DevboxMcp {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        Self {
            config,
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

const fn default_true() -> bool {
    true
}
const fn default_wait_timeout() -> f64 {
    60.0
}
const fn default_poll_ms() -> u64 {
    250
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
