use std::{
    path::{Path, PathBuf},
    pin::Pin,
    sync::Arc,
    task::{Context as TaskContext, Poll},
    time::Duration,
};

use anyhow::{Context, Result};
use axum::{
    body::{Body, Bytes},
    http::{HeaderMap, Method, StatusCode, Uri, header, request::Parts},
    response::Response,
};
use futures::{Stream, StreamExt as _};
use rmcp::{
    RoleServer,
    model::{CallToolResponse, CallToolResult, JsonObject},
    service::RequestContext,
};
use serde_json::{Map, Value, json};
use time::{OffsetDateTime, format_description::well_known::Rfc3339};
use tokio::{
    io::AsyncWriteExt as _,
    sync::{Mutex, mpsc},
};
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

use crate::{
    background::BackgroundTaskRegistry, oauth::OAuthRequestInfo,
    request_control::DisconnectCancellation,
};
use std::sync::atomic::{AtomicU64, Ordering};

const MAX_USAGE_PREVIEW_CHARS: usize = 240;
const MAX_TOOL_SUMMARY_CHARS: usize = 4_096;

#[derive(Debug)]
struct UsageLogState {
    bytes: Option<u64>,
}

#[derive(Debug)]
struct UsageLogSink {
    path: PathBuf,
    max_bytes: u64,
    rotations: usize,
    state: Mutex<UsageLogState>,
}

#[derive(Debug, Default)]
struct UsageQueueMetrics {
    enqueued: AtomicU64,
    dropped: AtomicU64,
    write_failures: AtomicU64,
}

#[derive(Debug)]
pub struct UsageLogger {
    #[cfg(test)]
    sink: Arc<UsageLogSink>,
    tx: mpsc::Sender<Value>,
    metrics: Arc<UsageQueueMetrics>,
    cancellation: Option<CancellationToken>,
}

impl UsageLogger {
    #[must_use]
    pub fn new(path: PathBuf, max_bytes: u64, rotations: usize) -> Self {
        let sink = Arc::new(UsageLogSink {
            path,
            max_bytes,
            rotations,
            state: Mutex::new(UsageLogState { bytes: None }),
        });
        let metrics = Arc::new(UsageQueueMetrics::default());
        let (tx, mut rx) = mpsc::channel::<Value>(1024);
        let writer_sink = sink.clone();
        let writer_metrics = metrics.clone();
        if let Ok(handle) = tokio::runtime::Handle::try_current() {
            handle.spawn(async move {
                while let Some(event) = rx.recv().await {
                    if writer_sink.append(&event).await.is_err() {
                        writer_metrics
                            .write_failures
                            .fetch_add(1, Ordering::Relaxed);
                    }
                }
            });
        } else {
            tracing::warn!(
                path = %sink.path.display(),
                "usage-log writer could not start because no Tokio runtime handle is available"
            );
        }
        Self {
            #[cfg(test)]
            sink,
            tx,
            metrics,
            cancellation: None,
        }
    }

    #[must_use]
    pub fn new_supervised(
        path: PathBuf,
        max_bytes: u64,
        rotations: usize,
        task_name: &'static str,
        background: &BackgroundTaskRegistry,
    ) -> Self {
        let sink = Arc::new(UsageLogSink {
            path,
            max_bytes,
            rotations,
            state: Mutex::new(UsageLogState { bytes: None }),
        });
        let metrics = Arc::new(UsageQueueMetrics::default());
        let (tx, rx) = mpsc::channel::<Value>(1024);
        let receiver = Arc::new(Mutex::new(rx));
        let writer_sink = sink.clone();
        let writer_metrics = metrics.clone();
        let cancellation = CancellationToken::new();
        background.spawn_supervised(
            task_name,
            cancellation.clone(),
            move |cancellation, heartbeat| {
                let receiver = receiver.clone();
                let sink = writer_sink.clone();
                let metrics = writer_metrics.clone();
                async move {
                    loop {
                        let event = {
                            let mut rx = receiver.lock().await;
                            tokio::select! {
                                () = cancellation.cancelled() => return Ok(()),
                                event = rx.recv() => event,
                            }
                        };
                        let Some(event) = event else {
                            cancellation.cancelled().await;
                            return Ok(());
                        };
                        heartbeat.attempt();
                        match sink.append(&event).await {
                            Ok(()) => heartbeat.tick(),
                            Err(error) => {
                                metrics.write_failures.fetch_add(1, Ordering::Relaxed);
                                heartbeat.fail(error.to_string());
                            }
                        }
                    }
                }
            },
        );
        Self {
            #[cfg(test)]
            sink,
            tx,
            metrics,
            cancellation: Some(cancellation),
        }
    }

    /// Queue telemetry without blocking the tool or HTTP request on filesystem I/O.
    pub fn enqueue(&self, event: Value) {
        match self.tx.try_send(event) {
            Ok(()) => {
                self.metrics.enqueued.fetch_add(1, Ordering::Relaxed);
            }
            Err(_) => {
                self.metrics.dropped.fetch_add(1, Ordering::Relaxed);
            }
        }
    }

    #[must_use]
    pub fn metrics_snapshot(&self) -> Value {
        json!({
            "enqueued": self.metrics.enqueued.load(Ordering::Relaxed),
            "dropped": self.metrics.dropped.load(Ordering::Relaxed),
            "writeFailures": self.metrics.write_failures.load(Ordering::Relaxed),
            "capacityEvents": self.tx.max_capacity(),
            "queuedEvents": self.tx.max_capacity().saturating_sub(self.tx.capacity()),
        })
    }

    /// Direct append retained only for deterministic rotation tests. Production callers
    /// must use [`Self::enqueue`] so telemetry can never block request/tool execution on disk I/O.
    ///
    /// # Errors
    /// Returns filesystem or serialization errors from the underlying usage-log sink.
    #[cfg(test)]
    pub async fn append(&self, event: &Value) -> Result<()> {
        self.sink.append(event).await
    }
}

impl Drop for UsageLogger {
    fn drop(&mut self) {
        if let Some(cancellation) = self.cancellation.as_ref() {
            cancellation.cancel();
        }
    }
}

impl UsageLogSink {
    /// Append one compact JSON object and rotate before crossing the configured byte limit.
    ///
    /// # Errors
    /// Returns filesystem, serialization, or directory-creation failures.
    pub async fn append(&self, event: &Value) -> Result<()> {
        let mut line = serde_json::to_vec(event).context("serialize usage event")?;
        line.push(b'\n');
        let line_bytes = u64::try_from(line.len()).unwrap_or(u64::MAX);
        let mut state = self.state.lock().await;
        if state.bytes.is_none() {
            if let Some(parent) = self.path.parent() {
                tokio::fs::create_dir_all(parent)
                    .await
                    .with_context(|| format!("create usage log directory {}", parent.display()))?;
            }
            state.bytes = Some(match tokio::fs::metadata(&self.path).await {
                Ok(metadata) => metadata.len(),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => 0,
                Err(error) => {
                    return Err(error)
                        .with_context(|| format!("stat usage log {}", self.path.display()));
                }
            });
        }
        let current = state.bytes.unwrap_or_default();
        if self.max_bytes > 0
            && self.rotations > 0
            && current.saturating_add(line_bytes) >= self.max_bytes
        {
            self.rotate().await?;
            state.bytes = Some(0);
        }
        let mut file = tokio::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .await
            .with_context(|| format!("open usage log {}", self.path.display()))?;
        file.write_all(&line)
            .await
            .with_context(|| format!("append usage log {}", self.path.display()))?;
        state.bytes = Some(state.bytes.unwrap_or_default().saturating_add(line_bytes));
        Ok(())
    }

    async fn rotate(&self) -> Result<()> {
        if self.rotations == 0 {
            return Ok(());
        }
        remove_if_exists(&rotation_path(&self.path, self.rotations)).await?;
        for index in (1..self.rotations).rev() {
            rename_if_exists(
                &rotation_path(&self.path, index),
                &rotation_path(&self.path, index + 1),
            )
            .await?;
        }
        rename_if_exists(&self.path, &rotation_path(&self.path, 1)).await
    }
}

#[derive(Debug)]
pub struct UsageService {
    tool: Arc<UsageLogger>,
    http: Arc<UsageLogger>,
}

impl UsageService {
    #[must_use]
    pub fn new(
        project_root: &Path,
        max_bytes: u64,
        rotations: usize,
        background: &BackgroundTaskRegistry,
    ) -> Self {
        let run = project_root.join("run");
        Self {
            tool: Arc::new(UsageLogger::new_supervised(
                run.join("tool-usage.jsonl"),
                max_bytes,
                rotations,
                "usage-tool-writer",
                background,
            )),
            http: Arc::new(UsageLogger::new_supervised(
                run.join("http-usage.jsonl"),
                max_bytes,
                rotations,
                "usage-http-writer",
                background,
            )),
        }
    }

    #[must_use]
    pub fn tool_logger(&self) -> Arc<UsageLogger> {
        self.tool.clone()
    }

    #[must_use]
    pub fn http_logger(&self) -> Arc<UsageLogger> {
        self.http.clone()
    }

    #[must_use]
    pub fn metrics_snapshot(&self) -> Value {
        json!({
            "tool": self.tool.metrics_snapshot(),
            "http": self.http.metrics_snapshot(),
        })
    }
}

#[derive(Debug)]
pub struct HttpUsageGuard {
    logger: Arc<UsageLogger>,
    request_id: String,
    started_at: String,
    started: std::time::Instant,
    method: String,
    path: String,
    accept: String,
    user_agent: String,
    forwarded_for: Option<String>,
    disconnect: Option<DisconnectCancellation>,
    completed: bool,
}

impl HttpUsageGuard {
    #[must_use]
    pub fn new(
        logger: Arc<UsageLogger>,
        method: &Method,
        uri: &Uri,
        headers: &HeaderMap,
        disconnect: Option<DisconnectCancellation>,
    ) -> Self {
        Self {
            logger,
            request_id: Uuid::new_v4().to_string(),
            started_at: utc_now(),
            started: std::time::Instant::now(),
            method: method.as_str().to_owned(),
            path: uri.path().to_owned(),
            accept: header_text(headers, header::ACCEPT),
            user_agent: header_text(headers, header::USER_AGENT),
            forwarded_for: headers
                .get("x-forwarded-for")
                .and_then(|value| value.to_str().ok())
                .and_then(|value| value.split(',').next())
                .map(str::trim)
                .filter(|value| !value.is_empty())
                .map(str::to_owned),
            disconnect,
            completed: false,
        }
    }

    #[must_use]
    pub fn wrap_response(self, response: Response) -> Response {
        let status = response.status();
        let (parts, body) = response.into_parts();
        let stream = LoggedBodyStream {
            inner: Some(body.into_data_stream().boxed()),
            usage: Some(self),
            status,
        };
        Response::from_parts(parts, Body::from_stream(stream))
    }

    fn finish_in_background(mut self, status: StatusCode) {
        self.completed = true;
        self.disconnect.take();
        let logger = self.logger.clone();
        let event = self.event("finished", Some(status));
        spawn_usage_append(&logger, event);
    }

    fn abort_in_background(&mut self) {
        if self.completed {
            return;
        }
        if let Some(disconnect) = self.disconnect.take() {
            let cancelled = disconnect.cancel();
            tracing::debug!(
                cancelled,
                "Cancelled MCP request after HTTP client disconnect"
            );
        }
        self.completed = true;
        let logger = self.logger.clone();
        let event = self.event("client_aborted", None);
        spawn_usage_append(&logger, event);
    }

    fn event(&self, outcome: &str, status: Option<StatusCode>) -> Value {
        json!({
            "type": "http_request",
            "request_id": self.request_id,
            "started_at": self.started_at,
            "finished_at": utc_now(),
            "duration_ms": elapsed_ms(self.started.elapsed()),
            "method": self.method,
            "path": self.path,
            "status_code": status.map(|value| value.as_u16()),
            "outcome": outcome,
            "client_aborted": outcome == "client_aborted",
            "accept": self.accept,
            "user_agent": self.user_agent,
            "forwarded_for": self.forwarded_for,
        })
    }
}

impl Drop for HttpUsageGuard {
    fn drop(&mut self) {
        self.abort_in_background();
    }
}

struct LoggedBodyStream {
    inner: Option<futures::stream::BoxStream<'static, Result<Bytes, axum::Error>>>,
    usage: Option<HttpUsageGuard>,
    status: StatusCode,
}

impl Stream for LoggedBodyStream {
    type Item = Result<Bytes, axum::Error>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut TaskContext<'_>) -> Poll<Option<Self::Item>> {
        let Some(inner) = self.inner.as_mut() else {
            return Poll::Ready(None);
        };
        match inner.poll_next_unpin(cx) {
            Poll::Ready(None) => {
                self.inner.take();
                if let Some(usage) = self.usage.take() {
                    usage.finish_in_background(self.status);
                }
                Poll::Ready(None)
            }
            other => other,
        }
    }
}

impl Drop for LoggedBodyStream {
    fn drop(&mut self) {
        let Some(usage) = self.usage.as_mut() else {
            return;
        };
        usage.abort_in_background();
        let Some(mut inner) = self.inner.take() else {
            return;
        };
        if let Ok(handle) = tokio::runtime::Handle::try_current() {
            handle.spawn(async move {
                let drain = async move { while inner.next().await.is_some() {} };
                let _ = tokio::time::timeout(Duration::from_secs(5), drain).await;
            });
        }
    }
}

fn spawn_usage_append(logger: &UsageLogger, event: Value) {
    logger.enqueue(event);
}

fn header_text(headers: &HeaderMap, name: header::HeaderName) -> String {
    headers
        .get(name)
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default()
        .to_owned()
}

#[derive(Debug, Clone)]
pub struct ToolUsageInvocation {
    pub invocation_id: String,
    pub tool: String,
    pub started_at: String,
    pub started: std::time::Instant,
    pub arguments: Value,
    pub context: Value,
}

pub struct ToolUsageDropGuard {
    logger: Arc<UsageLogger>,
    invocation: ToolUsageInvocation,
    completed: bool,
}

impl ToolUsageDropGuard {
    #[must_use]
    pub fn new(logger: Arc<UsageLogger>, invocation: ToolUsageInvocation) -> Self {
        Self {
            logger,
            invocation,
            completed: false,
        }
    }

    pub const fn complete(&mut self) {
        self.completed = true;
    }
}

impl Drop for ToolUsageDropGuard {
    fn drop(&mut self) {
        if self.completed {
            return;
        }
        spawn_usage_append(
            &self.logger,
            self.invocation
                .throw_event("MCP HTTP client disconnected before the tool result was delivered."),
        );
    }
}

impl ToolUsageInvocation {
    #[must_use]
    pub fn new(
        tool: &str,
        arguments: Option<&JsonObject>,
        context: &RequestContext<RoleServer>,
    ) -> Self {
        Self {
            invocation_id: Uuid::new_v4().to_string(),
            tool: tool.to_owned(),
            started_at: utc_now(),
            started: std::time::Instant::now(),
            arguments: summarize_tool_arguments(arguments),
            context: summarize_tool_context(context),
        }
    }

    #[must_use]
    pub fn start_event(&self) -> Value {
        json!({
            "type": "tool_start",
            "invocation_id": self.invocation_id,
            "tool": self.tool,
            "started_at": self.started_at,
            "arguments": self.arguments,
            "context": self.context,
        })
    }

    #[must_use]
    pub fn finish_event(&self, response: &CallToolResponse) -> Value {
        let elapsed = elapsed_ms(self.started.elapsed());
        let finished_at = utc_now();
        match response {
            CallToolResponse::Complete(result) => {
                finish_complete(self, result, &finished_at, elapsed)
            }
            CallToolResponse::InputRequired(_) => {
                finish_noncomplete(self, "Tool requested input.", &finished_at, elapsed)
            }
            CallToolResponse::Task(_) => {
                finish_noncomplete(self, "Tool materialized a task.", &finished_at, elapsed)
            }
            _ => finish_noncomplete(self, "Tool completed.", &finished_at, elapsed),
        }
    }

    #[must_use]
    pub fn throw_event(&self, error: &str) -> Value {
        let (error, truncated) = bounded_text(error, MAX_TOOL_SUMMARY_CHARS, "The command failed.");
        json!({
            "type": "tool_throw",
            "invocation_id": self.invocation_id,
            "tool": self.tool,
            "started_at": self.started_at,
            "finished_at": utc_now(),
            "duration_ms": elapsed_ms(self.started.elapsed()),
            "error": error,
            "error_truncated": truncated,
            "arguments": self.arguments,
            "context": self.context,
        })
    }
}

fn finish_complete(
    invocation: &ToolUsageInvocation,
    result: &CallToolResult,
    finished_at: &str,
    duration_ms: u64,
) -> Value {
    let structured = result.structured_content.as_ref().unwrap_or(&Value::Null);
    let (summary, summary_truncated) = bounded_text(
        structured
            .get("summary")
            .and_then(Value::as_str)
            .unwrap_or("Tool completed."),
        MAX_TOOL_SUMMARY_CHARS,
        "Tool completed.",
    );
    let data = structured.get("data").unwrap_or(&Value::Null);
    let result_text_chars = result
        .content
        .iter()
        .filter_map(|entry| entry.as_text())
        .map(|entry| utf16_len(&entry.text))
        .fold(0_usize, usize::saturating_add);
    json!({
        "type": "tool_finish",
        "invocation_id": invocation.invocation_id,
        "tool": invocation.tool,
        "started_at": invocation.started_at,
        "finished_at": finished_at,
        "duration_ms": duration_ms,
        "ok": structured.get("ok").and_then(Value::as_bool).unwrap_or(!result.is_error.unwrap_or(false)),
        "is_error": result.is_error.unwrap_or(false),
        "summary": if summary.is_empty() { Value::Null } else { Value::String(summary) },
        "summary_truncated": summary_truncated,
        "result_text_chars": result_text_chars,
        "stdout_chars": structured.get("stdout").and_then(Value::as_str).map_or(0, utf16_len),
        "stderr_chars": structured.get("stderr").and_then(Value::as_str).map_or(0, utf16_len),
        "exit_code": structured.get("exitCode").cloned().unwrap_or(Value::Null),
        "truncated": structured.get("truncated").and_then(Value::as_bool).unwrap_or(false),
        "queue_wait_ms": data.pointer("/execution/queue_wait_ms").cloned().unwrap_or(Value::Null),
        "execution_slot": data.pointer("/execution/slot").cloned().unwrap_or(Value::Null),
        "arguments": invocation.arguments,
        "context": invocation.context,
    })
}

fn finish_noncomplete(
    invocation: &ToolUsageInvocation,
    summary: &str,
    finished_at: &str,
    duration_ms: u64,
) -> Value {
    json!({
        "type": "tool_finish",
        "invocation_id": invocation.invocation_id,
        "tool": invocation.tool,
        "started_at": invocation.started_at,
        "finished_at": finished_at,
        "duration_ms": duration_ms,
        "ok": true,
        "is_error": false,
        "summary": summary,
        "summary_truncated": false,
        "result_text_chars": 0,
        "stdout_chars": 0,
        "stderr_chars": 0,
        "exit_code": Value::Null,
        "truncated": false,
        "queue_wait_ms": Value::Null,
        "execution_slot": Value::Null,
        "arguments": invocation.arguments,
        "context": invocation.context,
    })
}

#[must_use]
pub fn summarize_tool_arguments(arguments: Option<&JsonObject>) -> Value {
    let Some(arguments) = arguments else {
        return Value::Object(Map::new());
    };
    Value::Object(
        arguments
            .iter()
            .filter(|(key, _)| !is_internal_argument_key(key))
            .map(|(key, value)| (key.clone(), summarize_argument_value(key, value)))
            .collect(),
    )
}

fn summarize_argument_value(key: &str, value: &Value) -> Value {
    match value {
        Value::Null | Value::Bool(_) | Value::Number(_) => value.clone(),
        Value::String(text) => {
            let length = utf16_len(text);
            if is_sensitive_argument_key(key) {
                json!({ "type": "string", "length": length, "redacted": true })
            } else {
                json!({
                    "type": "string",
                    "length": length,
                    "preview": preview_utf16(text, MAX_USAGE_PREVIEW_CHARS),
                })
            }
        }
        Value::Array(values) => json!({
            "type": "array",
            "length": values.len(),
            "sample": values
                .iter()
                .take(8)
                .map(|value| summarize_argument_value(&format!("{key}[]"), value))
                .collect::<Vec<_>>(),
        }),
        Value::Object(object) => Value::Object(
            object
                .iter()
                .take(12)
                .map(|(nested, value)| {
                    (
                        nested.clone(),
                        summarize_argument_value(&format!("{key}.{nested}"), value),
                    )
                })
                .collect(),
        ),
    }
}

fn summarize_tool_context(context: &RequestContext<RoleServer>) -> Value {
    let mut result = Map::new();
    result.insert(
        "request_id".to_owned(),
        context.id.clone().into_json_value(),
    );
    if let Some(parts) = context.extensions.get::<Parts>() {
        if let Some(value) = parts
            .headers
            .get("mcp-session-id")
            .and_then(|value| value.to_str().ok())
            .filter(|value| !value.trim().is_empty())
        {
            result.insert("session_id".to_owned(), json!(value));
        }
        if let Some(value) = parts
            .headers
            .get(header::USER_AGENT)
            .and_then(|value| value.to_str().ok())
            .filter(|value| !value.trim().is_empty())
        {
            result.insert("user_agent".to_owned(), json!(value));
        }
        if let Some(auth) = parts.extensions.get::<OAuthRequestInfo>() {
            result.insert("client_id".to_owned(), json!(auth.client_id));
        }
    }
    Value::Object(result)
}

fn is_sensitive_argument_key(key: &str) -> bool {
    let key = key.to_ascii_lowercase();
    [
        "token",
        "secret",
        "password",
        "authorization",
        "cookie",
        "content_base64",
        "expected_sha256",
    ]
    .iter()
    .any(|needle| key.contains(needle))
}

fn is_internal_argument_key(key: &str) -> bool {
    matches!(
        key,
        "signal"
            | "sessionId"
            | "_meta"
            | "authInfo"
            | "requestId"
            | "requestInfo"
            | "taskId"
            | "taskStore"
            | "taskRequestedTtl"
            | "closeSSEStream"
            | "closeStandaloneSSEStream"
    )
}

fn preview_utf16(value: &str, max_units: usize) -> String {
    if utf16_len(value) <= max_units {
        return value.to_owned();
    }
    let mut used = 0_usize;
    let mut preview = String::new();
    for character in value.chars() {
        let units = character.len_utf16();
        if used.saturating_add(units) > max_units {
            break;
        }
        preview.push(character);
        used = used.saturating_add(units);
    }
    preview.push_str("...");
    preview
}

fn bounded_text(value: &str, max_units: usize, fallback: &str) -> (String, bool) {
    let value = if value.is_empty() { fallback } else { value };
    if utf16_len(value) <= max_units {
        return (value.to_owned(), false);
    }
    (
        preview_utf16(value, max_units)
            .trim_end_matches("...")
            .to_owned(),
        true,
    )
}

fn utf16_len(value: &str) -> usize {
    value.encode_utf16().count()
}

fn rotation_path(path: &Path, index: usize) -> PathBuf {
    PathBuf::from(format!("{}.{index}", path.display()))
}

async fn remove_if_exists(path: &Path) -> Result<()> {
    match tokio::fs::remove_file(path).await {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => {
            Err(error).with_context(|| format!("remove rotated usage log {}", path.display()))
        }
    }
}

async fn rename_if_exists(from: &Path, to: &Path) -> Result<()> {
    match tokio::fs::rename(from, to).await {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error)
            .with_context(|| format!("rotate usage log {} -> {}", from.display(), to.display())),
    }
}

fn utc_now() -> String {
    OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_else(|_| "1970-01-01T00:00:00Z".to_owned())
}

fn elapsed_ms(duration: Duration) -> u64 {
    u64::try_from(duration.as_millis()).unwrap_or(u64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn supervised_writer_stops_without_restart_when_logger_is_dropped() {
        let temp = tempfile::tempdir().unwrap();
        let background = BackgroundTaskRegistry::new();
        {
            let logger = UsageLogger::new_supervised(
                temp.path().join("usage.jsonl"),
                4096,
                1,
                "usage-test-writer",
                &background,
            );
            logger.enqueue(json!({"event": 1}));
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        tokio::time::sleep(Duration::from_millis(650)).await;
        let snapshot = background.snapshot();
        assert_eq!(snapshot["usage-test-writer"]["starts"], 1);
        assert_eq!(snapshot["usage-test-writer"]["running"], false);
    }

    #[test]
    fn sensitive_arguments_are_redacted_and_large_values_are_bounded() {
        let mut arguments = JsonObject::new();
        arguments.insert("token".to_owned(), json!("super-secret-value"));
        arguments.insert("secret".to_owned(), json!({"value": "nested-secret-value"}));
        arguments.insert("command".to_owned(), json!("x".repeat(300)));
        arguments.insert("requestId".to_owned(), json!(123));
        let summary = summarize_tool_arguments(Some(&arguments));
        assert_eq!(summary["token"]["redacted"], true);
        assert_eq!(summary["token"]["length"], 18);
        assert_eq!(summary["secret"]["value"]["redacted"], true);
        assert_eq!(summary["secret"]["value"]["length"], 19);
        assert_eq!(summary["command"]["length"], 300);
        assert_eq!(
            summary["command"]["preview"].as_str().map(str::len),
            Some(243)
        );
        assert!(summary.get("requestId").is_none());
        assert!(!summary.to_string().contains("super-secret-value"));
        assert!(!summary.to_string().contains("nested-secret-value"));
    }

    #[tokio::test]
    async fn telemetry_enqueue_is_bounded_and_never_waits_for_disk() {
        let temp = tempfile::tempdir().expect("temp");
        let sink = Arc::new(UsageLogSink {
            path: temp.path().join("usage.jsonl"),
            max_bytes: 1024,
            rotations: 1,
            state: Mutex::new(UsageLogState { bytes: None }),
        });
        let metrics = Arc::new(UsageQueueMetrics::default());
        let (tx, _rx) = mpsc::channel(1);
        let logger = UsageLogger {
            sink,
            tx,
            metrics,
            cancellation: None,
        };
        logger.enqueue(json!({"event": 1}));
        logger.enqueue(json!({"event": 2}));
        let snapshot = logger.metrics_snapshot();
        assert_eq!(snapshot["enqueued"], 1);
        assert_eq!(snapshot["dropped"], 1);
        assert_eq!(snapshot["queuedEvents"], 1);
    }

    #[tokio::test]
    async fn rotation_matches_javascript_threshold_and_order() {
        let temp = tempfile::tempdir().expect("temp");
        let path = temp.path().join("usage.jsonl");
        let logger = UsageLogger::new(path.clone(), 70, 2);
        logger
            .append(&json!({"event":"aaaaaaaaaaaaaaaaaaaa"}))
            .await
            .expect("first");
        logger
            .append(&json!({"event":"bbbbbbbbbbbbbbbbbbbb"}))
            .await
            .expect("second");
        logger
            .append(&json!({"event":"cccccccccccccccccccc"}))
            .await
            .expect("third");
        assert!(path.is_file());
        assert!(rotation_path(&path, 1).is_file());
        let current = tokio::fs::read_to_string(&path).await.expect("current");
        let first = tokio::fs::read_to_string(rotation_path(&path, 1))
            .await
            .expect("rotated");
        assert!(current.contains("cccc"));
        assert!(first.contains("aaaa") || first.contains("bbbb"));
    }
}
