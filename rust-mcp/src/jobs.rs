use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex, PoisonError},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use serde::Serialize;
use serde_json::{Map, Value, json};
use time::{OffsetDateTime, format_description::well_known::Rfc3339};
use tokio::{
    fs::{self, OpenOptions},
    io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt},
};
use tokio_util::sync::CancellationToken;

use crate::{execution::process_alive, process::terminate_process_tree};

const MAX_LOG_READ_CHARS: usize = 100_000;
const CHILD_IDENTITY_MAX_AGE: Duration = Duration::from_secs(60);

#[derive(Debug, Clone)]
pub struct JobStoreConfig {
    pub root: PathBuf,
    pub log_max_bytes: u64,
    pub log_rotations: usize,
    pub orphan_stale: Duration,
    pub retention: Duration,
    pub max_wait: Duration,
}

impl JobStoreConfig {
    #[must_use]
    pub fn normalized(mut self) -> Self {
        self.log_max_bytes = self.log_max_bytes.max(4096);
        self.orphan_stale = self.orphan_stale.max(Duration::from_secs(1));
        self.max_wait = self.max_wait.max(Duration::from_millis(100));
        self
    }
}

#[derive(Debug, Clone)]
pub struct JobPaths {
    pub id: String,
    pub dir: PathBuf,
    pub request: PathBuf,
    pub status: PathBuf,
    pub stdout: PathBuf,
    pub stderr: PathBuf,
    pub cancel: PathBuf,
    pub heartbeat: PathBuf,
}

#[derive(Debug, Clone, Serialize, Default)]
pub struct ReconcileSummary {
    /// Job directories discovered during this maintenance pass.
    pub discovered: u64,
    /// Jobs whose status was actually reconciled during this pass.
    pub scanned: u64,
    #[serde(rename = "batchLimited")]
    pub batch_limited: bool,
    pub interrupted: u64,
    pub active: u64,
    pub terminal: u64,
    pub maintained: u64,
    #[serde(rename = "compactedLogs")]
    pub compacted_logs: u64,
    pub deleted: u64,
    pub errors: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct LogSegment {
    pub index: usize,
    pub bytes: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct LogMetadata {
    #[serde(rename = "totalBytes")]
    pub total_bytes: u64,
    pub segments: Vec<LogSegment>,
    pub rotated: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct JobLogMetadata {
    pub stdout: LogMetadata,
    pub stderr: LogMetadata,
    #[serde(rename = "maxBytesPerSegment")]
    pub max_bytes_per_segment: u64,
    pub rotations: usize,
    pub truncated: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct JobLogs {
    pub id: String,
    pub status: String,
    pub stdout: String,
    pub stderr: String,
    #[serde(rename = "maxChars")]
    pub max_chars: usize,
    pub logs: JobLogMetadata,
}

#[derive(Debug, Clone, Serialize)]
pub struct LegacyCompaction {
    pub compacted: bool,
    pub bytes: u64,
    #[serde(rename = "previousBytes", skip_serializing_if = "Option::is_none")]
    pub previous_bytes: Option<u64>,
}

#[derive(Debug, Clone)]
pub struct JobStore {
    config: JobStoreConfig,
    maintenance_cursor: Arc<Mutex<Option<String>>>,
}

impl JobStore {
    #[must_use]
    pub fn new(config: JobStoreConfig) -> Self {
        Self {
            config: config.normalized(),
            maintenance_cursor: Arc::new(Mutex::new(None)),
        }
    }

    #[must_use]
    pub fn config(&self) -> &JobStoreConfig {
        &self.config
    }

    /// Create a new persisted job request/status directory and empty log files.
    ///
    /// # Errors
    /// Returns invalid-ID, serialization, or filesystem errors.
    pub async fn create_job(
        &self,
        job_id: &str,
        request: &Value,
        status: &Value,
    ) -> Result<JobPaths> {
        let paths = self.paths(job_id)?;
        fs::create_dir_all(&paths.dir).await?;
        let create = async {
            write_json_atomic(&paths.request, request).await?;
            write_json_atomic(&paths.status, status).await?;
            fs::write(&paths.stdout, []).await?;
            fs::write(&paths.stderr, []).await?;
            Ok::<(), anyhow::Error>(())
        }
        .await;
        if let Err(error) = create {
            fs::remove_dir_all(&paths.dir).await.ok();
            return Err(error);
        }
        Ok(paths)
    }

    /// Read the persisted request JSON without status reconciliation.
    ///
    /// # Errors
    /// Returns invalid-ID, filesystem, or JSON errors.
    pub async fn read_request(&self, job_id: &str) -> Result<Value> {
        read_json(&self.paths(job_id)?.request).await
    }

    /// Read raw status JSON without reconciliation.
    ///
    /// # Errors
    /// Returns invalid-ID, filesystem, or JSON errors.
    pub async fn read_status_raw(&self, job_id: &str) -> Result<Value> {
        read_json(&self.paths(job_id)?.status).await
    }

    /// Atomically persist raw job status JSON.
    ///
    /// # Errors
    /// Returns invalid-ID, serialization, or filesystem errors.
    pub async fn write_status(&self, job_id: &str, status: &Value) -> Result<()> {
        write_json_atomic(&self.paths(job_id)?.status, status).await
    }

    /// Atomically persist the current runner heartbeat.
    ///
    /// # Errors
    /// Returns invalid-ID, serialization, or filesystem errors.
    pub async fn write_heartbeat(&self, job_id: &str, heartbeat: &Value) -> Result<()> {
        write_json_atomic(&self.paths(job_id)?.heartbeat, heartbeat).await
    }

    /// Return whether cancellation has been requested for a job.
    ///
    /// # Errors
    /// Returns invalid-ID or filesystem errors.
    pub async fn cancellation_requested(&self, job_id: &str) -> Result<bool> {
        marker_exists(&self.paths(job_id)?.cancel).await
    }

    /// Resolve validated job paths below the configured job root.
    ///
    /// # Errors
    /// Returns an error for IDs outside the JavaScript MCP job-id contract.
    pub fn paths(&self, job_id: &str) -> Result<JobPaths> {
        let id = validate_job_id(job_id)?;
        let dir = self.config.root.join(&id);
        Ok(JobPaths {
            id,
            request: dir.join("request.json"),
            status: dir.join("status.json"),
            stdout: dir.join("stdout.log"),
            stderr: dir.join("stderr.log"),
            cancel: dir.join("cancel.requested"),
            heartbeat: dir.join("heartbeat.json"),
            dir,
        })
    }

    /// Read and reconcile one job status, including stale-runner/orphan handling.
    ///
    /// # Errors
    /// Returns filesystem, JSON, process-inspection, or invalid-ID errors.
    pub async fn get_status(&self, job_id: &str) -> Result<Value> {
        let paths = self.paths(job_id)?;
        let value = read_json(&paths.status).await?;
        self.reconcile_status(&paths, value).await
    }

    async fn reconcile_status(&self, paths: &JobPaths, value: Value) -> Result<Value> {
        let Some(mut object) = value.as_object().cloned() else {
            bail!(
                "Job status {} is not a JSON object.",
                paths.status.display()
            );
        };
        let status = string_field(&object, "status").unwrap_or_default();
        if is_terminal(status) {
            decorate_status(&mut object, paths, false, None);
            return Ok(Value::Object(object));
        }

        if marker_exists(&paths.cancel).await? {
            return self.reconcile_cancelled(paths, object).await;
        }

        let heartbeat = read_heartbeat(paths).await?;
        let heartbeat_age = heartbeat.age;
        let status_age = status_age(&object);
        let heartbeat_stale = heartbeat_age.map_or(status_age >= self.config.orphan_stale, |age| {
            age >= self.config.orphan_stale
        });
        let runner_pid = u32_field(&object, "runnerPid");
        // A fresh heartbeat is stronger and cheaper evidence of runner liveness than
        // interrogating the Windows process table on every status poll. Only fall
        // back to an OS liveness check once the heartbeat is stale.
        let runner_alive = match (runner_pid, heartbeat_stale) {
            (Some(_), false) => true,
            (Some(pid), true) => process_alive(pid).await,
            (None, _) => false,
        };
        if !runner_alive && heartbeat_stale {
            return self
                .interrupt_orphan(paths, object, heartbeat.value, heartbeat_age)
                .await;
        }
        decorate_status(&mut object, paths, runner_alive, heartbeat_age);
        Ok(Value::Object(object))
    }

    async fn reconcile_cancelled(
        &self,
        paths: &JobPaths,
        mut object: Map<String, Value>,
    ) -> Result<Value> {
        let runner_pid = u32_field(&object, "runnerPid");
        let runner_alive = match runner_pid {
            Some(pid) => process_alive(pid).await,
            None => false,
        };
        object.insert("status".to_owned(), json!("cancelled"));
        object.insert("cancelRequested".to_owned(), json!(true));
        if object.get("completedAtUtc").is_none_or(Value::is_null) {
            object.insert("completedAtUtc".to_owned(), json!(utc_now()));
        }
        if !runner_alive {
            write_json_atomic(&paths.status, &Value::Object(object.clone())).await?;
        }
        decorate_status(&mut object, paths, runner_alive, None);
        Ok(Value::Object(object))
    }

    async fn interrupt_orphan(
        &self,
        paths: &JobPaths,
        mut object: Map<String, Value>,
        heartbeat: Option<Value>,
        heartbeat_age: Option<Duration>,
    ) -> Result<Value> {
        let child_pid = heartbeat
            .as_ref()
            .and_then(|value| value.get("childPid"))
            .and_then(Value::as_u64)
            .and_then(|pid| u32::try_from(pid).ok())
            .or_else(|| u32_field(&object, "childPid"));
        let runtime_mode = string_field(&object, "runtimeMode")
            .map(str::to_owned)
            .or_else(|| {
                heartbeat
                    .as_ref()
                    .and_then(|value| value.get("runtimeMode"))
                    .and_then(Value::as_str)
                    .map(str::to_owned)
            })
            .unwrap_or_else(|| "host".to_owned());
        let cleanup = cleanup_orphan_child(child_pid, heartbeat_age, &runtime_mode).await;

        object.insert("status".to_owned(), json!("interrupted"));
        if object.get("completedAtUtc").is_none_or(Value::is_null) {
            object.insert("completedAtUtc".to_owned(), json!(utc_now()));
        }
        object.insert("interrupted".to_owned(), json!(true));
        object.insert("runtimeMode".to_owned(), json!(runtime_mode));
        object.insert(
            "orphanChildTerminated".to_owned(),
            json!(cleanup.child_terminated),
        );
        object.insert(
            "orphanDockerClientTerminated".to_owned(),
            json!(cleanup.docker_client_terminated),
        );
        object.insert(
            "orphanChildCleanupSkipped".to_owned(),
            cleanup.skipped.map_or(Value::Null, Value::String),
        );
        if let Some(pid) = child_pid {
            object.insert("childPid".to_owned(), json!(pid));
        }
        object.entry("error").or_insert_with(|| {
            json!("Detached job runner disappeared before recording a terminal status.")
        });
        if let Some(age) = heartbeat_age {
            object.insert("heartbeatAgeMs".to_owned(), json!(duration_ms(age)));
        } else {
            object.insert("heartbeatAgeMs".to_owned(), Value::Null);
        }
        write_json_atomic(&paths.status, &Value::Object(object.clone())).await?;
        decorate_status(&mut object, paths, false, heartbeat_age);
        Ok(Value::Object(object))
    }

    /// Reconcile every known job, compact one-time legacy oversized logs, and apply retention.
    ///
    /// # Errors
    /// Returns an error only when the jobs root itself cannot be created/read. Per-job failures are counted.
    pub async fn reconcile_all(&self) -> Result<ReconcileSummary> {
        self.reconcile_maintenance_batch(usize::MAX).await
    }

    /// Reconcile a bounded slice of known jobs so maintenance cost cannot grow linearly
    /// with the entire retained history on every minute tick.
    ///
    /// The directory names are cheap to enumerate, while status/log inspection is capped
    /// to `max_jobs`. The cursor is shared by all clones in this MCP process so successive
    /// passes advance through the retained set instead of repeatedly starting at job zero.
    ///
    /// # Errors
    /// Returns an error when the jobs root cannot be enumerated. Per-job failures are counted.
    pub async fn reconcile_maintenance_batch(&self, max_jobs: usize) -> Result<ReconcileSummary> {
        fs::create_dir_all(&self.config.root).await?;
        let mut reader = fs::read_dir(&self.config.root).await?;
        let mut ids = Vec::new();
        while let Some(entry) = reader.next_entry().await? {
            let name = entry.file_name().to_string_lossy().into_owned();
            if validate_job_id(&name).is_ok() {
                ids.push(name);
            }
        }
        ids.sort_unstable();
        let mut summary = ReconcileSummary {
            discovered: u64::try_from(ids.len()).unwrap_or(u64::MAX),
            ..ReconcileSummary::default()
        };
        if ids.is_empty() || max_jobs == 0 {
            return Ok(summary);
        }

        let cursor = self
            .maintenance_cursor
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone();
        let start = cursor
            .as_deref()
            .and_then(|value| ids.iter().position(|id| id.as_str() > value))
            .unwrap_or(0);
        let limit = max_jobs.min(ids.len());
        let end = start.saturating_add(limit).min(ids.len());
        for id in &ids[start..end] {
            summary.scanned = summary.scanned.saturating_add(1);
            if self
                .reconcile_one_for_maintenance(id, &mut summary)
                .await
                .is_err()
            {
                summary.errors = summary.errors.saturating_add(1);
            }
        }
        summary.batch_limited = end < ids.len();
        let next_cursor = if end >= ids.len() {
            None
        } else {
            ids.get(end.saturating_sub(1)).cloned()
        };
        *self
            .maintenance_cursor
            .lock()
            .unwrap_or_else(PoisonError::into_inner) = next_cursor;
        Ok(summary)
    }

    async fn reconcile_one_for_maintenance(
        &self,
        job_id: &str,
        summary: &mut ReconcileSummary,
    ) -> Result<()> {
        let status = self.get_status(job_id).await?;
        let status_name = status
            .get("status")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if status_name == "interrupted" {
            summary.interrupted = summary.interrupted.saturating_add(1);
        } else if is_terminal(status_name) {
            summary.terminal = summary.terminal.saturating_add(1);
        } else {
            summary.active = summary.active.saturating_add(1);
            return Ok(());
        }
        let paths = self.paths(job_id)?;
        if self.retention_expired(&status) {
            fs::remove_dir_all(&paths.dir).await?;
            summary.deleted = summary.deleted.saturating_add(1);
            return Ok(());
        }
        let mut raw = read_json(&paths.status).await?;
        if raw.get("maintenanceReconciledAtUtc").is_some() {
            return Ok(());
        }
        let (stdout, stderr) = tokio::try_join!(
            compact_legacy_log(&paths.stdout, self.config.log_max_bytes),
            compact_legacy_log(&paths.stderr, self.config.log_max_bytes)
        )?;
        let compacted = stdout.compacted || stderr.compacted;
        if compacted {
            summary.compacted_logs = summary.compacted_logs.saturating_add(1);
        }
        if let Some(object) = raw.as_object_mut() {
            object.insert("maintenanceReconciledAtUtc".to_owned(), json!(utc_now()));
            object.insert("legacyLogsCompacted".to_owned(), json!(compacted));
        }
        write_json_atomic(&paths.status, &raw).await?;
        summary.maintained = summary.maintained.saturating_add(1);
        Ok(())
    }

    fn retention_expired(&self, status: &Value) -> bool {
        if self.config.retention.is_zero() {
            return false;
        }
        let timestamp = status
            .get("completedAtUtc")
            .and_then(Value::as_str)
            .or_else(|| status.get("createdAtUtc").and_then(Value::as_str));
        timestamp.and_then(parse_utc).is_some_and(|timestamp| {
            let age = OffsetDateTime::now_utc() - timestamp;
            age >= time::Duration::ZERO && age >= time_duration(self.config.retention)
        })
    }

    /// Long-poll a job status without occupying a command execution slot.
    ///
    /// # Errors
    /// Returns filesystem/JSON errors or cancellation.
    pub async fn wait_status(
        &self,
        job_id: &str,
        wait: Duration,
        terminal_only: bool,
        poll: Duration,
        cancellation: &CancellationToken,
    ) -> Result<Value> {
        let bounded = wait.min(self.config.max_wait);
        if bounded.is_zero() {
            return self.get_status(job_id).await;
        }
        let started = tokio::time::Instant::now();
        let deadline = started + bounded;
        let first = tokio::time::timeout_at(deadline, self.get_status(job_id))
            .await
            .map_err(|_| anyhow::anyhow!("Job status wait exceeded its {bounded:?} deadline while reading the initial state."))??;
        if is_terminal(status_name(&first)) {
            return Ok(first);
        }
        let initial = status_name(&first).to_owned();
        let mut current = first;
        loop {
            let now = tokio::time::Instant::now();
            if now >= deadline {
                break;
            }
            let delay = poll.max(Duration::from_millis(50)).min(deadline - now);
            tokio::select! {
                () = tokio::time::sleep(delay) => {},
                () = cancellation.cancelled() => bail!("Job status wait cancelled by the MCP client."),
            }
            current = match tokio::time::timeout_at(deadline, self.get_status(job_id)).await {
                Ok(result) => result?,
                Err(_) => break,
            };
            let name = status_name(&current);
            if is_terminal(name) || (!terminal_only && name != initial) {
                return Ok(current);
            }
        }
        if let Some(object) = current.as_object_mut() {
            object.insert("waitTimedOut".to_owned(), json!(true));
            object.insert("waitedMs".to_owned(), json!(duration_ms(bounded)));
        }
        Ok(current)
    }

    /// Read bounded stdout/stderr tails across the configured rotation chain.
    ///
    /// # Errors
    /// Returns invalid-ID, filesystem, or status errors.
    pub async fn logs(&self, job_id: &str, max_chars: usize) -> Result<JobLogs> {
        let paths = self.paths(job_id)?;
        let bounded = max_chars.clamp(100, MAX_LOG_READ_CHARS);
        let (stdout, stderr, status, stdout_meta, stderr_meta) = tokio::try_join!(
            read_tail_across_rotations(&paths.stdout, bounded, self.config.log_rotations),
            read_tail_across_rotations(&paths.stderr, bounded, self.config.log_rotations),
            self.get_status(job_id),
            log_metadata(&paths.stdout, self.config.log_rotations),
            log_metadata(&paths.stderr, self.config.log_rotations)
        )?;
        let rotated = stdout_meta.rotated || stderr_meta.rotated;
        Ok(JobLogs {
            id: paths.id,
            status: status_name(&status).to_owned(),
            stdout,
            stderr,
            max_chars: bounded,
            logs: JobLogMetadata {
                stdout: stdout_meta,
                stderr: stderr_meta,
                max_bytes_per_segment: self.config.log_max_bytes,
                rotations: self.config.log_rotations,
                truncated: rotated,
            },
        })
    }

    /// Request cancellation and terminate the detached runner process tree.
    ///
    /// # Errors
    /// Returns invalid-ID, filesystem, status, or process-cleanup errors.
    pub async fn cancel(&self, job_id: &str) -> Result<Value> {
        let paths = self.paths(job_id)?;
        let status = self.get_status(job_id).await?;
        let already_terminal = is_terminal(status_name(&status));
        let runner_alive = status
            .get("runnerAlive")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        if already_terminal && !(status_name(&status) == "cancelled" && runner_alive) {
            return Ok(status);
        }
        let mut cancelled = status.as_object().cloned().unwrap_or_default();
        let completed = utc_now();
        cancelled.insert("status".to_owned(), json!("cancelled"));
        cancelled.insert("completedAtUtc".to_owned(), json!(completed.clone()));
        cancelled.insert("cancelRequested".to_owned(), json!(true));
        create_marker_once(&paths.cancel, &format!("{completed}\n")).await?;
        if let Some(pid) = u32_field(&cancelled, "runnerPid").filter(|_| runner_alive) {
            terminate_job_runner_gracefully(pid, &paths.status).await;
        }
        cancelled.insert("runnerAlive".to_owned(), json!(false));
        cancelled.insert("jobDir".to_owned(), json!(path_text(&paths.dir)));
        Ok(Value::Object(cancelled))
    }
}

#[cfg(windows)]
async fn terminate_job_runner_gracefully(pid: u32, _status_path: &Path) {
    terminate_process_tree(pid).await;
}

#[cfg(unix)]
async fn terminate_job_runner_gracefully(pid: u32, status_path: &Path) {
    use nix::{
        sys::signal::{Signal, killpg},
        unistd::Pid,
    };

    let Ok(raw_pid) = i32::try_from(pid) else {
        return;
    };
    let group = Pid::from_raw(raw_pid);
    let _ = killpg(group, Signal::SIGTERM);
    let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
    loop {
        if read_json(status_path)
            .await
            .ok()
            .is_some_and(|status| is_terminal(status_name(&status)) && status.get("logs").is_some())
        {
            return;
        }
        if tokio::time::Instant::now() >= deadline {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let _ = killpg(group, Signal::SIGKILL);
}

#[cfg(not(any(windows, unix)))]
async fn terminate_job_runner_gracefully(pid: u32, _status_path: &Path) {
    terminate_process_tree(pid).await;
}

#[derive(Debug)]
struct HeartbeatState {
    value: Option<Value>,
    age: Option<Duration>,
}

#[derive(Debug)]
struct OrphanCleanup {
    child_terminated: bool,
    docker_client_terminated: bool,
    skipped: Option<String>,
}

async fn cleanup_orphan_child(
    child_pid: Option<u32>,
    heartbeat_age: Option<Duration>,
    runtime_mode: &str,
) -> OrphanCleanup {
    let mut result = OrphanCleanup {
        child_terminated: false,
        docker_client_terminated: false,
        skipped: None,
    };
    let Some(pid) = child_pid else {
        return result;
    };
    if !process_alive(pid).await {
        return result;
    }
    if heartbeat_age.is_none_or(|age| age > CHILD_IDENTITY_MAX_AGE) {
        result.skipped = Some("heartbeat-too-old-to-safely-trust-reused-pid".to_owned());
        return result;
    }
    terminate_process_tree(pid).await;
    let terminated = !process_alive(pid).await;
    if runtime_mode == "docker" {
        result.docker_client_terminated = terminated;
        result.skipped = Some("docker-container-exec-not-force-killed-shared-container".to_owned());
    } else {
        result.child_terminated = terminated;
    }
    result
}

fn validate_job_id(job_id: &str) -> Result<String> {
    let value = job_id.trim();
    let valid_length = (8..=81).contains(&value.len());
    let mut bytes = value.bytes();
    let valid_first = bytes
        .next()
        .is_some_and(|byte| byte.is_ascii_alphanumeric());
    let valid_rest = bytes.all(|byte| byte.is_ascii_alphanumeric() || byte == b'-');
    if !valid_length || !valid_first || !valid_rest {
        bail!("Invalid Devbox job id.");
    }
    Ok(value.to_owned())
}

async fn read_json(path: &Path) -> Result<Value> {
    let mut last_error = None;
    for attempt in 0..3 {
        match fs::read(path).await {
            Ok(bytes) => match serde_json::from_slice(&bytes) {
                Ok(value) => return Ok(value),
                Err(error) => {
                    last_error = Some(
                        anyhow::Error::new(error)
                            .context(format!("parse job JSON {}", path.display())),
                    );
                }
            },
            Err(error) => {
                last_error = Some(
                    anyhow::Error::new(error).context(format!("read job JSON {}", path.display())),
                );
            }
        }
        if attempt < 2 {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    }
    Err(last_error.unwrap_or_else(|| anyhow::anyhow!("failed to read job JSON {}", path.display())))
}

async fn write_json_atomic(path: &Path, value: &Value) -> Result<()> {
    let parent = path.parent().context("job JSON path has no parent")?;
    fs::create_dir_all(parent).await?;
    let temp = parent.join(format!(
        ".{}.{}.tmp",
        path.file_name()
            .and_then(|name| name.to_str())
            .unwrap_or("job"),
        unique_suffix()
    ));
    let mut bytes = serde_json::to_vec_pretty(value)?;
    bytes.push(b'\n');
    fs::write(&temp, bytes).await?;
    match fs::rename(&temp, path).await {
        Ok(()) => Ok(()),
        Err(error)
            if matches!(
                error.kind(),
                std::io::ErrorKind::AlreadyExists | std::io::ErrorKind::PermissionDenied
            ) =>
        {
            fs::remove_file(path).await.ok();
            fs::rename(&temp, path).await?;
            Ok(())
        }
        Err(error) => {
            fs::remove_file(&temp).await.ok();
            Err(error.into())
        }
    }
}

async fn read_heartbeat(paths: &JobPaths) -> Result<HeartbeatState> {
    let metadata = match fs::metadata(&paths.heartbeat).await {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(HeartbeatState {
                value: None,
                age: None,
            });
        }
        Err(error) => return Err(error.into()),
    };
    let age = metadata
        .modified()
        .ok()
        .and_then(|modified| modified.elapsed().ok());
    let value = read_json(&paths.heartbeat).await.ok();
    Ok(HeartbeatState { value, age })
}

async fn marker_exists(path: &Path) -> Result<bool> {
    match fs::metadata(path).await {
        Ok(_) => Ok(true),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error.into()),
    }
}

async fn create_marker_once(path: &Path, content: &str) -> Result<()> {
    let result = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .await;
    match result {
        Ok(mut file) => {
            file.write_all(content.as_bytes()).await?;
            file.flush().await?;
            Ok(())
        }
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => Ok(()),
        Err(error) => Err(error.into()),
    }
}

async fn read_tail(path: &Path, max_chars: usize) -> Result<String> {
    let metadata = match fs::metadata(path).await {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(String::new()),
        Err(error) => return Err(error.into()),
    };
    let target_bytes = max_chars.saturating_mul(4).max(4096);
    let bytes = metadata
        .len()
        .min(u64::try_from(target_bytes).unwrap_or(u64::MAX));
    let mut file = fs::File::open(path).await?;
    file.seek(std::io::SeekFrom::Start(
        metadata.len().saturating_sub(bytes),
    ))
    .await?;
    let mut buffer = Vec::with_capacity(usize::try_from(bytes).unwrap_or(usize::MAX));
    file.read_to_end(&mut buffer).await?;
    let text = String::from_utf8_lossy(&buffer).into_owned();
    Ok(utf16_tail(&text, max_chars))
}

async fn read_tail_across_rotations(
    path: &Path,
    max_chars: usize,
    rotations: usize,
) -> Result<String> {
    let mut remaining = max_chars.max(1);
    let mut chunks = Vec::new();
    for index in 0..=rotations {
        if remaining == 0 {
            break;
        }
        let candidate = rotated_path(path, index);
        let text = read_tail(&candidate, remaining).await?;
        if text.is_empty() {
            continue;
        }
        remaining = remaining.saturating_sub(text.encode_utf16().count());
        chunks.insert(0, text);
    }
    Ok(utf16_tail(&chunks.concat(), max_chars))
}

async fn log_metadata(path: &Path, rotations: usize) -> Result<LogMetadata> {
    let mut segments = Vec::new();
    let mut total_bytes = 0_u64;
    for index in 0..=rotations {
        let candidate = rotated_path(path, index);
        match fs::metadata(&candidate).await {
            Ok(metadata) => {
                segments.push(LogSegment {
                    index,
                    bytes: metadata.len(),
                });
                total_bytes = total_bytes.saturating_add(metadata.len());
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
    }
    let rotated = segments.iter().any(|segment| segment.index > 0);
    Ok(LogMetadata {
        total_bytes,
        segments,
        rotated,
    })
}

async fn compact_legacy_log(path: &Path, max_bytes: u64) -> Result<LegacyCompaction> {
    let metadata = match fs::metadata(path).await {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(LegacyCompaction {
                compacted: false,
                bytes: 0,
                previous_bytes: None,
            });
        }
        Err(error) => return Err(error.into()),
    };
    let limit = max_bytes.max(4096).min(usize::MAX as u64);
    if metadata.len() <= limit {
        return Ok(LegacyCompaction {
            compacted: false,
            bytes: metadata.len(),
            previous_bytes: None,
        });
    }
    let mut file = fs::File::open(path).await?;
    file.seek(std::io::SeekFrom::Start(metadata.len() - limit))
        .await?;
    let mut buffer = Vec::with_capacity(usize::try_from(limit).unwrap_or(usize::MAX));
    file.read_to_end(&mut buffer).await?;
    fs::write(path, &buffer).await?;
    Ok(LegacyCompaction {
        compacted: true,
        bytes: u64::try_from(buffer.len()).unwrap_or(u64::MAX),
        previous_bytes: Some(metadata.len()),
    })
}

fn rotated_path(path: &Path, index: usize) -> PathBuf {
    if index == 0 {
        path.to_path_buf()
    } else {
        PathBuf::from(format!("{}.{index}", path_text(path)))
    }
}

fn decorate_status(
    object: &mut Map<String, Value>,
    paths: &JobPaths,
    runner_alive: bool,
    heartbeat_age: Option<Duration>,
) {
    object.insert("runnerAlive".to_owned(), json!(runner_alive));
    if let Some(age) = heartbeat_age {
        object.insert("heartbeatAgeMs".to_owned(), json!(duration_ms(age)));
    }
    object.insert("jobDir".to_owned(), json!(path_text(&paths.dir)));
}

fn status_age(object: &Map<String, Value>) -> Duration {
    for key in ["startedAtUtc", "queuedAtUtc", "createdAtUtc"] {
        if let Some(value) = string_field(object, key)
            && let Some(timestamp) = parse_utc(value)
        {
            let delta = OffsetDateTime::now_utc() - timestamp;
            if delta > time::Duration::ZERO {
                return std_duration(delta);
            }
        }
    }
    Duration::MAX
}

fn status_name(value: &Value) -> &str {
    value
        .get("status")
        .and_then(Value::as_str)
        .unwrap_or_default()
}

fn is_terminal(status: &str) -> bool {
    matches!(
        status,
        "succeeded" | "failed" | "cancelled" | "timed_out" | "interrupted"
    )
}

fn string_field<'a>(object: &'a Map<String, Value>, key: &str) -> Option<&'a str> {
    object.get(key).and_then(Value::as_str)
}

fn u32_field(object: &Map<String, Value>, key: &str) -> Option<u32> {
    object
        .get(key)
        .and_then(Value::as_u64)
        .and_then(|value| u32::try_from(value).ok())
}

fn parse_utc(value: &str) -> Option<OffsetDateTime> {
    OffsetDateTime::parse(value, &Rfc3339).ok()
}

fn utc_now() -> String {
    OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_else(|_| unique_suffix())
}

fn unique_suffix() -> String {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    format!("{}-{nanos}", std::process::id())
}

fn duration_ms(duration: Duration) -> u64 {
    u64::try_from(duration.as_millis()).unwrap_or(u64::MAX)
}

fn time_duration(duration: Duration) -> time::Duration {
    time::Duration::try_from(duration).unwrap_or(time::Duration::MAX)
}

fn std_duration(duration: time::Duration) -> Duration {
    Duration::try_from(duration).unwrap_or(Duration::MAX)
}

fn utf16_tail(text: &str, max_units: usize) -> String {
    if text.encode_utf16().count() <= max_units {
        return text.to_owned();
    }
    let mut units = 0_usize;
    let mut start = text.len();
    for (index, character) in text.char_indices().rev() {
        let width = character.len_utf16();
        if units.saturating_add(width) > max_units {
            break;
        }
        units = units.saturating_add(width);
        start = index;
    }
    text[start..].to_owned()
}

fn path_text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn store(root: &Path) -> JobStore {
        JobStore::new(JobStoreConfig {
            root: root.to_path_buf(),
            log_max_bytes: 4096,
            log_rotations: 2,
            orphan_stale: Duration::from_millis(1),
            retention: Duration::from_secs(1),
            max_wait: Duration::from_secs(2),
        })
    }

    async fn write_status(store: &JobStore, id: &str, value: Value) -> JobPaths {
        let paths = store.paths(id).unwrap();
        fs::create_dir_all(&paths.dir).await.unwrap();
        write_json_atomic(&paths.status, &value).await.unwrap();
        paths
    }

    #[test]
    fn job_id_validation_matches_js_contract() {
        assert!(validate_job_id("job-abc12345").is_ok());
        assert!(validate_job_id("A2345678").is_ok());
        assert!(validate_job_id("short").is_err());
        assert!(validate_job_id("job/escape123").is_err());
        assert!(validate_job_id("-job-abc123").is_err());
    }

    #[tokio::test]
    async fn dead_stale_runner_is_reconciled_to_interrupted() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        let id = "job-orphan123";
        write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "running",
                "createdAtUtc": "2020-01-01T00:00:00Z",
                "startedAtUtc": "2020-01-01T00:00:00Z",
                "runnerPid": u32::MAX,
                "runtimeMode": "host"
            }),
        )
        .await;
        let status = store.get_status(id).await.unwrap();
        assert_eq!(status["status"], "interrupted");
        assert_eq!(status["runnerAlive"], false);
        assert_eq!(status["interrupted"], true);
    }

    #[tokio::test]
    async fn orphan_cleanup_does_not_trust_child_pid_without_fresh_heartbeat() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        let id = "job-pidsafe12";
        write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "running",
                "createdAtUtc": "2020-01-01T00:00:00Z",
                "startedAtUtc": "2020-01-01T00:00:00Z",
                "runnerPid": u32::MAX,
                "childPid": std::process::id(),
                "runtimeMode": "host"
            }),
        )
        .await;
        let status = store.get_status(id).await.unwrap();
        assert_eq!(status["status"], "interrupted");
        assert_eq!(status["orphanChildTerminated"], false);
        assert_eq!(
            status["orphanChildCleanupSkipped"],
            "heartbeat-too-old-to-safely-trust-reused-pid"
        );
        assert!(process_alive(std::process::id()).await);
    }

    #[tokio::test]
    async fn log_tail_spans_rotations_in_chronological_order() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        let id = "job-logs1234";
        let paths = write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "succeeded",
                "createdAtUtc": utc_now(),
                "completedAtUtc": utc_now()
            }),
        )
        .await;
        fs::write(format!("{}.2", path_text(&paths.stdout)), "oldest-")
            .await
            .unwrap();
        fs::write(format!("{}.1", path_text(&paths.stdout)), "middle-")
            .await
            .unwrap();
        fs::write(&paths.stdout, "newest").await.unwrap();
        fs::write(&paths.stderr, "warning").await.unwrap();
        let logs = store.logs(id, 1000).await.unwrap();
        assert_eq!(logs.stdout, "oldest-middle-newest");
        assert_eq!(logs.stderr, "warning");
        assert!(logs.logs.stdout.rotated);
        assert!(logs.logs.truncated);
    }

    #[tokio::test]
    async fn long_poll_observes_status_change_without_process_sleep() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        let id = "job-wait1234";
        let paths = write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "queued",
                "createdAtUtc": utc_now(),
                "runnerPid": std::process::id()
            }),
        )
        .await;
        let update_path = paths.status.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(40)).await;
            write_json_atomic(
                &update_path,
                &json!({
                    "id": id,
                    "status": "running",
                    "createdAtUtc": utc_now(),
                    "runnerPid": std::process::id()
                }),
            )
            .await
            .unwrap();
        });
        let cancellation = CancellationToken::new();
        let status = store
            .wait_status(
                id,
                Duration::from_secs(1),
                false,
                Duration::from_millis(20),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(status["status"], "running");
    }

    #[tokio::test]
    async fn long_poll_timeout_is_a_hard_deadline() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        let id = "job-deadline123";
        write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "running",
                "createdAtUtc": utc_now(),
                "runnerPid": std::process::id()
            }),
        )
        .await;
        let started = std::time::Instant::now();
        let status = store
            .wait_status(
                id,
                Duration::from_millis(120),
                true,
                Duration::from_millis(20),
                &CancellationToken::new(),
            )
            .await
            .unwrap();
        assert_eq!(status["waitTimedOut"], true);
        assert!(started.elapsed() >= Duration::from_millis(100));
        assert!(started.elapsed() < Duration::from_millis(500));
    }

    #[tokio::test]
    async fn maintenance_reconciles_bounded_rotating_batches() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        for index in 0..5 {
            let id = format!("job-batch{index:04}");
            write_status(
                &store,
                &id,
                json!({
                    "id": id,
                    "status": "succeeded",
                    "createdAtUtc": utc_now(),
                    "completedAtUtc": utc_now(),
                }),
            )
            .await;
        }
        let first = store.reconcile_maintenance_batch(2).await.unwrap();
        let second = store.reconcile_maintenance_batch(2).await.unwrap();
        let third = store.reconcile_maintenance_batch(2).await.unwrap();
        assert_eq!(first.discovered, 5);
        assert_eq!(first.scanned, 2);
        assert_eq!(second.scanned, 2);
        assert_eq!(third.scanned, 1);
        assert!(first.batch_limited);
        assert!(!third.batch_limited);
    }

    #[tokio::test]
    async fn retention_removes_expired_terminal_job() {
        let temp = tempfile::tempdir().unwrap();
        let store = store(temp.path());
        let id = "job-old12345";
        let paths = write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "succeeded",
                "createdAtUtc": "2020-01-01T00:00:00Z",
                "completedAtUtc": "2020-01-01T00:00:00Z"
            }),
        )
        .await;
        let summary = store.reconcile_all().await.unwrap();
        assert_eq!(summary.deleted, 1);
        assert!(!paths.dir.exists());
    }

    #[tokio::test]
    async fn legacy_log_compaction_keeps_bounded_tail() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("stdout.log");
        let mut data = vec![b'a'; 5000];
        data.extend_from_slice(b"THE-END");
        fs::write(&path, data).await.unwrap();
        let result = compact_legacy_log(&path, 4096).await.unwrap();
        assert!(result.compacted);
        assert_eq!(fs::metadata(&path).await.unwrap().len(), 4096);
        let bytes = fs::read(&path).await.unwrap();
        assert!(bytes.ends_with(b"THE-END"));
    }

    #[tokio::test]
    async fn cancellation_marker_is_idempotent_and_status_is_cancelled() {
        let temp = tempfile::tempdir().unwrap();
        let mut config = store(temp.path()).config().clone();
        config.orphan_stale = Duration::from_secs(60);
        let store = JobStore::new(config);
        let id = "job-cancel123";
        let paths = write_status(
            &store,
            id,
            json!({
                "id": id,
                "status": "running",
                "createdAtUtc": utc_now(),
                "runnerPid": u32::MAX
            }),
        )
        .await;
        let first = store.cancel(id).await.unwrap();
        let second = store.cancel(id).await.unwrap();
        let reconciled = store.get_status(id).await.unwrap();
        assert_eq!(first["status"], "cancelled");
        assert_eq!(second["status"], "cancelled");
        assert_eq!(reconciled["status"], "cancelled");
        assert!(reconciled["completedAtUtc"].as_str().is_some());
        assert!(paths.cancel.exists());
    }
}
