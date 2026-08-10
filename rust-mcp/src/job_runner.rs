use std::{
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU32, Ordering},
    },
    time::Duration,
};

use anyhow::{Context, Result, bail};
use serde_json::{Value, json};
use time::{OffsetDateTime, format_description::well_known::Rfc3339};
use tokio::{sync::RwLock, task::JoinHandle};
use tokio_util::sync::CancellationToken;

use crate::{
    Config,
    execution::{
        AcquireRequest, ExecutionLease, ExecutionScheduler, ResourceClass, SchedulerConfig,
    },
    job_logs::{JobLogPump, JobLogSnapshots},
    job_manager::{JobMode, JobRequest, job_store_config},
    jobs::JobStore,
    runtime::{ProgramRequest, RuntimeExecError, RuntimeExecutor, ShellRequest},
};

#[derive(Clone)]
struct HeartbeatState {
    store: JobStore,
    job_id: String,
    runtime_mode: String,
    status: Arc<RwLock<String>>,
    child_pid: Arc<AtomicU32>,
}

impl HeartbeatState {
    async fn write(&self) {
        let status = self.status.read().await.clone();
        let child_pid = self.child_pid.load(Ordering::Relaxed);
        let heartbeat = json!({
            "pid": std::process::id(),
            "status": status,
            "childPid": (child_pid > 0).then_some(child_pid),
            "runtimeMode": self.runtime_mode,
            "updatedAtUtc": utc_now(),
        });
        self.store
            .write_heartbeat(&self.job_id, &heartbeat)
            .await
            .ok();
    }

    async fn set_status(&self, status: &str) {
        {
            let mut current = self.status.write().await;
            status.clone_into(&mut *current);
        }
        self.write().await;
    }

    async fn set_child_pid(&self, pid: u32) {
        self.child_pid.store(pid, Ordering::Relaxed);
        self.write().await;
    }

    fn child_pid(&self) -> Option<u32> {
        let pid = self.child_pid.load(Ordering::Relaxed);
        (pid > 0).then_some(pid)
    }
}

struct RunnerMonitor {
    heartbeat: HeartbeatState,
    cancellation: CancellationToken,
    stop: CancellationToken,
    task: JoinHandle<()>,
}

impl RunnerMonitor {
    async fn start(store: JobStore, request: &JobRequest, heartbeat_ms: u64) -> Self {
        let heartbeat = HeartbeatState {
            store,
            job_id: request.id.clone(),
            runtime_mode: request.runtime_mode.clone(),
            status: Arc::new(RwLock::new("queued".to_owned())),
            child_pid: Arc::new(AtomicU32::new(0)),
        };
        heartbeat.write().await;
        let cancellation = CancellationToken::new();
        let stop = CancellationToken::new();
        let task_heartbeat = heartbeat.clone();
        let task_cancel = cancellation.clone();
        let task_stop = stop.clone();
        let task = tokio::spawn(async move {
            let mut cancel_poll = tokio::time::interval(Duration::from_millis(250));
            cancel_poll.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            let mut heartbeat_tick =
                tokio::time::interval(Duration::from_millis(heartbeat_ms.max(1000)));
            heartbeat_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            let shutdown_signal = wait_runner_shutdown_signal();
            tokio::pin!(shutdown_signal);
            loop {
                tokio::select! {
                    () = task_stop.cancelled() => break,
                    () = &mut shutdown_signal => {
                        task_cancel.cancel();
                        break;
                    }
                    _ = cancel_poll.tick() => {
                        if task_heartbeat
                            .store
                            .cancellation_requested(&task_heartbeat.job_id)
                            .await
                            .unwrap_or(false)
                        {
                            task_cancel.cancel();
                        }
                    }
                    _ = heartbeat_tick.tick() => task_heartbeat.write().await,
                }
            }
        });
        Self {
            heartbeat,
            cancellation,
            stop,
            task,
        }
    }

    #[must_use]
    fn cancellation(&self) -> CancellationToken {
        self.cancellation.clone()
    }

    async fn set_status(&self, status: &str) {
        self.heartbeat.set_status(status).await;
    }

    fn child_pid(&self) -> Option<u32> {
        self.heartbeat.child_pid()
    }

    async fn finish(self, status: &str) {
        self.heartbeat.set_status(status).await;
        self.stop.cancel();
        self.task.await.ok();
    }
}

/// Execute one persisted detached Rust job request.
///
/// This mode is called by the Rust MCP executable itself with `--job-runner` and is
/// intentionally independent of the HTTP/auth server lifecycle.
///
/// # Errors
/// Returns request-validation, filesystem, scheduling, log, or runtime errors that
/// prevent a trustworthy terminal status from being persisted.
pub async fn run_job_request(config: Arc<Config>, request_path: &Path) -> Result<()> {
    let Some(prepared) = prepare_runner(config, request_path).await? else {
        return Ok(());
    };
    run_prepared(prepared).await
}

#[cfg(unix)]
async fn wait_runner_shutdown_signal() {
    use tokio::signal::unix::{SignalKind, signal};

    let Ok(mut terminate) = signal(SignalKind::terminate()) else {
        std::future::pending::<()>().await;
        return;
    };
    let Ok(mut interrupt) = signal(SignalKind::interrupt()) else {
        std::future::pending::<()>().await;
        return;
    };
    tokio::select! {
        _ = terminate.recv() => {},
        _ = interrupt.recv() => {},
    }
}

#[cfg(not(unix))]
async fn wait_runner_shutdown_signal() {
    std::future::pending::<()>().await;
}

struct PreparedRunner {
    config: Arc<Config>,
    request: JobRequest,
    store: JobStore,
    queued_at_utc: String,
    monitor: RunnerMonitor,
    log_pump: JobLogPump,
    scheduler: ExecutionScheduler,
    weight: usize,
}

async fn prepare_runner(
    config: Arc<Config>,
    request_path: &Path,
) -> Result<Option<PreparedRunner>> {
    let request_bytes = tokio::fs::read(request_path)
        .await
        .with_context(|| format!("read Rust job request {}", request_path.display()))?;
    let request: JobRequest = serde_json::from_slice(&request_bytes)
        .with_context(|| format!("parse Rust job request {}", request_path.display()))?;
    let store = JobStore::new(job_store_config(&config));
    validate_request_path(&store, &request, request_path).await?;
    let initial = store.read_status_raw(&request.id).await.ok();
    let already_cancelled = initial
        .as_ref()
        .and_then(|value| value.get("status"))
        .and_then(Value::as_str)
        == Some("cancelled")
        || store.cancellation_requested(&request.id).await?;
    if already_cancelled {
        return Ok(None);
    }

    let queued_at_utc = utc_now();
    store
        .write_status(&request.id, &queued_status(&request, &queued_at_utc))
        .await?;
    let monitor = RunnerMonitor::start(store.clone(), &request, config.job_heartbeat_ms).await;
    let paths = store.paths(&request.id)?;
    let log_pump = JobLogPump::start(
        paths.stdout,
        paths.stderr,
        config.job_log_max_bytes,
        config.job_log_rotations,
    )
    .await?;
    let scheduler = ExecutionScheduler::new(SchedulerConfig {
        root: config.execution_slot_root.clone(),
        max_concurrent: config.exec_max_concurrent,
        reserved_interactive: config.exec_reserved_interactive,
        watch_max_concurrent: config.watch_max_concurrent,
        queue_timeout: Duration::from_millis(config.background_queue_timeout_ms),
        heavy_weight: config.exec_heavy_weight,
    });
    let weight = if request.resource_class == ResourceClass::Heavy {
        config.exec_heavy_weight.max(1)
    } else {
        1
    };
    Ok(Some(PreparedRunner {
        config,
        request,
        store,
        queued_at_utc,
        monitor,
        log_pump,
        scheduler,
        weight,
    }))
}

async fn run_prepared(prepared: PreparedRunner) -> Result<()> {
    let PreparedRunner {
        config,
        request,
        store,
        queued_at_utc,
        monitor,
        log_pump,
        scheduler,
        weight,
    } = prepared;
    let cancellation = monitor.cancellation();
    let lease_result = scheduler
        .acquire(
            AcquireRequest::background(
                format!("devbox_job:{}", request.id),
                request.resource_class,
                weight,
            ),
            &cancellation,
        )
        .await;
    let mut lease = match lease_result {
        Ok(lease) => lease,
        Err(error) => {
            return finish_queue_failure(
                &request,
                &store,
                &queued_at_utc,
                monitor,
                log_pump,
                &cancellation,
                weight,
                &error.to_string(),
            )
            .await;
        }
    };

    if cancellation.is_cancelled() || store.cancellation_requested(&request.id).await? {
        lease.release().await.ok();
        let logs = log_pump.finish().await?;
        let status = cancelled_status(
            &request,
            &queued_at_utc,
            Some(&lease),
            &logs,
            monitor.child_pid(),
        );
        persist_terminal_unless_cancelled(&store, &request.id, &status).await?;
        monitor.finish("cancelled").await;
        return Ok(());
    }

    monitor.set_status("running").await;
    let started_at_utc = utc_now();
    store
        .write_status(
            &request.id,
            &running_status(&request, &queued_at_utc, &started_at_utc, &lease, None),
        )
        .await?;
    let runtime_result =
        execute_runtime(config, &request, &log_pump, &monitor, cancellation.clone()).await;
    let child_pid = monitor.child_pid();
    lease.release().await.ok();
    let logs = log_pump.finish().await?;
    let externally_cancelled = cancellation.is_cancelled()
        || store
            .cancellation_requested(&request.id)
            .await
            .unwrap_or(false);
    let (status_name, final_status) = terminal_from_runtime(
        &request,
        &queued_at_utc,
        &started_at_utc,
        &lease,
        child_pid,
        runtime_result,
        externally_cancelled,
        &logs,
    );
    persist_terminal_unless_cancelled(&store, &request.id, &final_status).await?;
    monitor.finish(status_name).await;
    Ok(())
}

#[allow(
    clippy::too_many_arguments,
    reason = "queue failure finalization requires the persisted job schema context"
)]
async fn finish_queue_failure(
    request: &JobRequest,
    store: &JobStore,
    queued_at_utc: &str,
    monitor: RunnerMonitor,
    log_pump: JobLogPump,
    cancellation: &CancellationToken,
    weight: usize,
    error: &str,
) -> Result<()> {
    let logs = log_pump.finish().await?;
    let cancelled = cancellation.is_cancelled()
        || store
            .cancellation_requested(&request.id)
            .await
            .unwrap_or(false);
    let status_name = if cancelled { "cancelled" } else { "failed" };
    let final_status =
        failed_before_execution_status(request, queued_at_utc, status_name, error, weight, &logs);
    persist_terminal_unless_cancelled(store, &request.id, &final_status).await?;
    monitor.finish(status_name).await;
    Ok(())
}

async fn validate_request_path(
    store: &JobStore,
    request: &JobRequest,
    request_path: &Path,
) -> Result<()> {
    let expected = store.paths(&request.id)?.request;
    let expected = canonical_or_lexical(&expected).await;
    let supplied = canonical_or_lexical(request_path).await;
    if expected != supplied {
        bail!(
            "Rust job request path {} does not match configured job root path {}.",
            request_path.display(),
            expected.display()
        );
    }
    Ok(())
}

async fn canonical_or_lexical(path: &Path) -> PathBuf {
    tokio::fs::canonicalize(path)
        .await
        .unwrap_or_else(|_| path.to_path_buf())
}

async fn execute_runtime(
    config: Arc<Config>,
    request: &JobRequest,
    log_pump: &JobLogPump,
    monitor: &RunnerMonitor,
    cancellation: CancellationToken,
) -> Result<crate::process::ProcessOutput, RuntimeExecError> {
    let runtime = RuntimeExecutor::new(config.clone());
    let (pid_tx, mut pid_rx) = tokio::sync::mpsc::unbounded_channel::<u32>();
    let heartbeat = monitor.heartbeat.clone();
    let pid_task = tokio::spawn(async move {
        if let Some(pid) = pid_rx.recv().await {
            heartbeat.set_child_pid(pid).await;
        }
    });
    let output_tx = log_pump.sender();
    let working_dir = resolve_working_dir(&config, request);
    let timeout = Duration::from_millis(request.timeout_ms);
    let result = match request.mode {
        JobMode::Program => {
            runtime
                .run_program(
                    ProgramRequest {
                        program: request.program.clone().unwrap_or_default(),
                        args: request.args.clone(),
                        input: request.input.clone().map(String::into_bytes),
                        working_dir,
                        timeout,
                        user: effective_user(&config, request),
                        max_capture_chars: Some(65_536),
                        output_tx,
                        pid_tx: Some(pid_tx),
                    },
                    cancellation,
                )
                .await
        }
        JobMode::Shell => {
            runtime
                .run_shell(
                    ShellRequest {
                        command: request.command.clone().unwrap_or_default(),
                        working_dir,
                        timeout,
                        user: effective_user(&config, request),
                        max_capture_chars: Some(65_536),
                        output_tx,
                        pid_tx: Some(pid_tx),
                    },
                    cancellation,
                )
                .await
        }
    };
    pid_task.await.ok();
    result
}

fn resolve_working_dir(config: &Config, request: &JobRequest) -> PathBuf {
    if !request.working_dir.trim().is_empty() {
        return PathBuf::from(request.working_dir.trim());
    }
    if config.runtime_mode == crate::RuntimeMode::Host {
        config.host_default_workdir.clone()
    } else {
        config.devbox_workspace_path.clone()
    }
}

fn effective_user(config: &Config, request: &JobRequest) -> String {
    if request.user.trim().is_empty() {
        config.devbox_default_user.clone()
    } else {
        request.user.trim().to_owned()
    }
}

fn queued_status(request: &JobRequest, queued_at_utc: &str) -> Value {
    json!({
        "id": request.id,
        "status": "queued",
        "mode": request.mode.as_str(),
        "createdAtUtc": request.created_at_utc,
        "queuedAtUtc": queued_at_utc,
        "startedAtUtc": null,
        "completedAtUtc": null,
        "runnerPid": std::process::id(),
        "exitCode": null,
        "readOnly": request.read_only,
        "resourceClass": request.resource_class.as_str(),
        "runtimeMode": request.runtime_mode,
    })
}

fn running_status(
    request: &JobRequest,
    queued_at_utc: &str,
    started_at_utc: &str,
    lease: &ExecutionLease,
    child_pid: Option<u32>,
) -> Value {
    json!({
        "id": request.id,
        "status": "running",
        "mode": request.mode.as_str(),
        "createdAtUtc": request.created_at_utc,
        "queuedAtUtc": queued_at_utc,
        "startedAtUtc": started_at_utc,
        "completedAtUtc": null,
        "runnerPid": std::process::id(),
        "exitCode": null,
        "readOnly": request.read_only,
        "resourceClass": request.resource_class.as_str(),
        "runtimeMode": request.runtime_mode,
        "queueWaitMs": lease.queue_wait_ms,
        "executionSlot": lease.slot,
        "executionSlots": lease.slots,
        "executionPool": lease.pool,
        "executionWeight": lease.weight,
        "childPid": child_pid,
    })
}

fn failed_before_execution_status(
    request: &JobRequest,
    queued_at_utc: &str,
    status: &str,
    error: &str,
    weight: usize,
    logs: &JobLogSnapshots,
) -> Value {
    json!({
        "id": request.id,
        "status": status,
        "mode": request.mode.as_str(),
        "createdAtUtc": request.created_at_utc,
        "queuedAtUtc": queued_at_utc,
        "startedAtUtc": null,
        "completedAtUtc": utc_now(),
        "runnerPid": std::process::id(),
        "exitCode": null,
        "readOnly": request.read_only,
        "resourceClass": request.resource_class.as_str(),
        "runtimeMode": request.runtime_mode,
        "queueWaitMs": null,
        "executionSlot": null,
        "executionSlots": null,
        "executionPool": null,
        "executionWeight": weight,
        "childPid": null,
        "error": error,
        "logs": logs,
    })
}

fn cancelled_status(
    request: &JobRequest,
    queued_at_utc: &str,
    lease: Option<&ExecutionLease>,
    logs: &JobLogSnapshots,
    child_pid: Option<u32>,
) -> Value {
    json!({
        "id": request.id,
        "status": "cancelled",
        "mode": request.mode.as_str(),
        "createdAtUtc": request.created_at_utc,
        "queuedAtUtc": queued_at_utc,
        "startedAtUtc": null,
        "completedAtUtc": utc_now(),
        "runnerPid": std::process::id(),
        "exitCode": null,
        "readOnly": request.read_only,
        "resourceClass": request.resource_class.as_str(),
        "runtimeMode": request.runtime_mode,
        "queueWaitMs": lease.map(|value| value.queue_wait_ms),
        "executionSlot": lease.and_then(|value| value.slot),
        "executionSlots": lease.map(|value| value.slots.clone()),
        "executionPool": lease.map(|value| value.pool.clone()),
        "executionWeight": lease.map(|value| value.weight),
        "childPid": child_pid,
        "logs": logs,
    })
}

#[allow(
    clippy::too_many_arguments,
    reason = "terminal status mirrors the persisted detached-job schema"
)]
fn terminal_from_runtime(
    request: &JobRequest,
    queued_at_utc: &str,
    started_at_utc: &str,
    lease: &ExecutionLease,
    child_pid: Option<u32>,
    result: Result<crate::process::ProcessOutput, RuntimeExecError>,
    externally_cancelled: bool,
    logs: &JobLogSnapshots,
) -> (&'static str, Value) {
    match result {
        Ok(output) => (
            "succeeded",
            json!({
                "id": request.id,
                "status": "succeeded",
                "mode": request.mode.as_str(),
                "createdAtUtc": request.created_at_utc,
                "queuedAtUtc": queued_at_utc,
                "startedAtUtc": started_at_utc,
                "completedAtUtc": utc_now(),
                "runnerPid": std::process::id(),
                "exitCode": output.exit_code,
                "readOnly": request.read_only,
                "resourceClass": request.resource_class.as_str(),
                "runtimeMode": request.runtime_mode,
                "queueWaitMs": lease.queue_wait_ms,
                "executionSlot": lease.slot,
                "executionSlots": lease.slots,
                "executionPool": lease.pool,
                "executionWeight": lease.weight,
                "childPid": child_pid,
                "logs": logs,
            }),
        ),
        Err(error) => {
            let process = match &error {
                RuntimeExecError::Process(process) => Some(process),
                _ => None,
            };
            let cancelled = externally_cancelled || process.is_some_and(|value| value.aborted);
            let timed_out = process.is_some_and(|value| value.timed_out);
            let status = if cancelled {
                "cancelled"
            } else if timed_out {
                "timed_out"
            } else {
                "failed"
            };
            (
                status,
                json!({
                    "id": request.id,
                    "status": status,
                    "mode": request.mode.as_str(),
                    "createdAtUtc": request.created_at_utc,
                    "queuedAtUtc": queued_at_utc,
                    "startedAtUtc": started_at_utc,
                    "completedAtUtc": utc_now(),
                    "runnerPid": std::process::id(),
                    "exitCode": process.and_then(|value| value.exit_code),
                    "readOnly": request.read_only,
                    "resourceClass": request.resource_class.as_str(),
                    "runtimeMode": request.runtime_mode,
                    "queueWaitMs": lease.queue_wait_ms,
                    "executionSlot": lease.slot,
                    "executionSlots": lease.slots,
                    "executionPool": lease.pool,
                    "executionWeight": lease.weight,
                    "childPid": child_pid,
                    "error": error.to_string(),
                    "logs": logs,
                }),
            )
        }
    }
}

async fn persist_terminal_unless_cancelled(
    store: &JobStore,
    job_id: &str,
    final_status: &Value,
) -> Result<()> {
    let current = store.read_status_raw(job_id).await.ok();
    if current
        .as_ref()
        .and_then(|value| value.get("status"))
        .and_then(Value::as_str)
        != Some("cancelled")
    {
        store.write_status(job_id, final_status).await?;
    }
    Ok(())
}

fn utc_now() -> String {
    OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_else(|_| "1970-01-01T00:00:00Z".to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> JobRequest {
        JobRequest {
            id: "job-runner123".to_owned(),
            mode: JobMode::Program,
            command: None,
            program: Some("rustc".to_owned()),
            args: vec!["--version".to_owned()],
            input: None,
            working_dir: String::new(),
            timeout_ms: 10_000,
            user: String::new(),
            read_only: false,
            resource_class: ResourceClass::Light,
            runtime_mode: "host".to_owned(),
            created_at_utc: utc_now(),
        }
    }

    #[tokio::test]
    async fn heartbeat_status_transition_does_not_self_deadlock() {
        let temp = tempfile::tempdir().unwrap();
        let store = JobStore::new(crate::jobs::JobStoreConfig {
            root: temp.path().to_path_buf(),
            log_max_bytes: 4096,
            log_rotations: 1,
            orphan_stale: Duration::from_secs(5),
            retention: Duration::ZERO,
            max_wait: Duration::from_secs(1),
        });
        let state = HeartbeatState {
            store,
            job_id: "job-locktest1".to_owned(),
            runtime_mode: "host".to_owned(),
            status: Arc::new(RwLock::new("queued".to_owned())),
            child_pid: Arc::new(AtomicU32::new(0)),
        };
        tokio::time::timeout(Duration::from_secs(1), state.set_status("running"))
            .await
            .expect("heartbeat status transition must not deadlock");
        assert_eq!(*state.status.read().await, "running");
    }

    #[test]
    fn successful_terminal_status_preserves_scheduler_and_log_metadata() {
        let request = request();
        let temp = tempfile::tempdir().unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 1,
            reserved_interactive: 0,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_secs(1),
            heavy_weight: 2,
        });
        let runtime = tokio::runtime::Runtime::new().unwrap();
        runtime.block_on(async {
            let cancellation = CancellationToken::new();
            let mut lease = scheduler
                .acquire(
                    AcquireRequest::background("test", ResourceClass::Light, 1),
                    &cancellation,
                )
                .await
                .unwrap();
            let logs = JobLogSnapshots {
                stdout: crate::job_logs::RotatingLogSnapshot {
                    max_bytes: 4096,
                    rotations: 2,
                    rotations_performed: 0,
                    total_bytes: 10,
                    current_bytes: 10,
                    truncated: false,
                    failed: false,
                    error: None,
                },
                stderr: crate::job_logs::RotatingLogSnapshot {
                    max_bytes: 4096,
                    rotations: 2,
                    rotations_performed: 0,
                    total_bytes: 0,
                    current_bytes: 0,
                    truncated: false,
                    failed: false,
                    error: None,
                },
                truncated: false,
            };
            let output = crate::process::ProcessOutput {
                stdout: "rustc".to_owned(),
                stderr: String::new(),
                exit_code: 0,
                pid: 42,
                elapsed_ms: 1,
            };
            let (name, status) = terminal_from_runtime(
                &request,
                &utc_now(),
                &utc_now(),
                &lease,
                Some(42),
                Ok(output),
                false,
                &logs,
            );
            assert_eq!(name, "succeeded");
            assert_eq!(status["executionWeight"], 1);
            assert_eq!(status["logs"]["stdout"]["totalBytes"], 10);

            let cancelled_error = RuntimeExecError::Process(crate::process::ProcessError {
                message: "Command cancelled by the MCP client.".to_owned(),
                exit_code: None,
                stdout: "".into(),
                stderr: "".into(),
                file: "node".into(),
                args: Vec::<String>::new().into_boxed_slice(),
                timed_out: false,
                aborted: true,
                signal: None,
                elapsed_ms: 1,
            });
            let (cancelled_name, cancelled) = terminal_from_runtime(
                &request,
                &utc_now(),
                &utc_now(),
                &lease,
                Some(42),
                Err(cancelled_error),
                true,
                &logs,
            );
            assert_eq!(cancelled_name, "cancelled");
            assert_eq!(cancelled["error"], "Command cancelled by the MCP client.");
            assert_eq!(cancelled["logs"]["stdout"]["totalBytes"], 10);
            assert!(cancelled.get("cancelRequested").is_none());
            lease.release().await.unwrap();
        });
    }

    #[test]
    fn process_timeout_and_abort_map_to_distinct_terminal_states() {
        let error = |timed_out, aborted| {
            RuntimeExecError::Process(crate::process::ProcessError {
                message: "failure".to_owned(),
                exit_code: None,
                stdout: "".into(),
                stderr: "".into(),
                file: "test".into(),
                args: Vec::<String>::new().into_boxed_slice(),
                timed_out,
                aborted,
                signal: None,
                elapsed_ms: 1,
            })
        };
        assert!(matches!(error(true, false), RuntimeExecError::Process(_)));
        assert!(matches!(error(false, true), RuntimeExecError::Process(_)));
    }
}
