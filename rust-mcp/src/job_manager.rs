use std::{
    process::Stdio,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::json;
use time::{OffsetDateTime, format_description::well_known::Rfc3339};
use tokio::process::Command;

use crate::{
    Config,
    execution::ResourceClass,
    jobs::{JobPaths, JobStore, JobStoreConfig},
};

static JOB_COUNTER: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum JobMode {
    Shell,
    Program,
}

impl JobMode {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Shell => "shell",
            Self::Program => "program",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct JobRequest {
    pub id: String,
    pub mode: JobMode,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub command: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub program: Option<String>,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub input: Option<String>,
    #[serde(default)]
    pub working_dir: String,
    pub timeout_ms: u64,
    #[serde(default)]
    pub user: String,
    #[serde(default)]
    pub read_only: bool,
    pub resource_class: ResourceClass,
    pub runtime_mode: String,
    pub created_at_utc: String,
}

#[derive(Debug, Clone)]
pub struct StartShellJob {
    pub command: String,
    pub working_dir: String,
    pub timeout: Duration,
    pub user: String,
    pub read_only: bool,
    pub resource_class: String,
}

#[derive(Debug, Clone)]
pub struct StartProgramJob {
    pub program: String,
    pub args: Vec<String>,
    pub input: Option<String>,
    pub working_dir: String,
    pub timeout: Duration,
    pub user: String,
    pub resource_class: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct JobStartSummary {
    pub id: String,
    pub status: String,
    pub runner_pid: Option<u32>,
    pub job_dir: String,
    pub mode: String,
    pub resource_class: String,
}

#[derive(Debug, Clone)]
pub struct JobManager {
    config: Arc<Config>,
    store: JobStore,
}

impl JobManager {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        let store = JobStore::new(job_store_config(&config));
        Self { config, store }
    }

    #[must_use]
    pub fn store(&self) -> &JobStore {
        &self.store
    }

    /// Persist and launch a detached shell job.
    ///
    /// # Errors
    /// Returns filesystem, serialization, executable-discovery, or process-launch errors.
    pub async fn start_shell(&self, options: StartShellJob) -> Result<JobStartSummary> {
        let request = self.shell_request(options);
        self.persist_and_spawn(request).await
    }

    /// Persist and launch a detached direct-program job.
    ///
    /// # Errors
    /// Returns filesystem, serialization, executable-discovery, or process-launch errors.
    pub async fn start_program(&self, options: StartProgramJob) -> Result<JobStartSummary> {
        let request = self.program_request(options);
        self.persist_and_spawn(request).await
    }

    fn shell_request(&self, options: StartShellJob) -> JobRequest {
        let text = options.command.clone();
        JobRequest {
            id: new_job_id(),
            mode: JobMode::Shell,
            command: Some(options.command),
            program: None,
            args: Vec::new(),
            input: None,
            working_dir: options.working_dir,
            timeout_ms: bounded_timeout_ms(options.timeout),
            user: options.user,
            read_only: options.read_only,
            resource_class: infer_resource_class(&text, &options.resource_class),
            runtime_mode: self.config.runtime_mode.as_str().to_owned(),
            created_at_utc: utc_now(),
        }
    }

    fn program_request(&self, options: StartProgramJob) -> JobRequest {
        let text = format!("{} {}", options.program, options.args.join(" "));
        JobRequest {
            id: new_job_id(),
            mode: JobMode::Program,
            command: None,
            program: Some(options.program),
            args: options.args,
            input: options.input,
            working_dir: options.working_dir,
            timeout_ms: bounded_timeout_ms(options.timeout),
            user: options.user,
            read_only: false,
            resource_class: infer_resource_class(&text, &options.resource_class),
            runtime_mode: self.config.runtime_mode.as_str().to_owned(),
            created_at_utc: utc_now(),
        }
    }

    async fn persist_and_spawn(&self, request: JobRequest) -> Result<JobStartSummary> {
        let initial = json!({
            "id": request.id,
            "status": "queued",
            "mode": request.mode.as_str(),
            "createdAtUtc": request.created_at_utc,
            "startedAtUtc": null,
            "completedAtUtc": null,
            "runnerPid": null,
            "exitCode": null,
            "readOnly": request.read_only,
            "resourceClass": request.resource_class.as_str(),
            "runtimeMode": request.runtime_mode,
        });
        let request_value = serde_json::to_value(&request)?;
        let paths = self
            .store
            .create_job(&request.id, &request_value, &initial)
            .await?;
        let runner_pid = match spawn_detached_runner(&self.config, &paths) {
            Ok(pid) => pid,
            Err(error) => {
                let failed = json!({
                    "id": request.id,
                    "status": "failed",
                    "mode": request.mode.as_str(),
                    "createdAtUtc": request.created_at_utc,
                    "startedAtUtc": null,
                    "completedAtUtc": utc_now(),
                    "runnerPid": null,
                    "exitCode": null,
                    "readOnly": request.read_only,
                    "resourceClass": request.resource_class.as_str(),
                    "runtimeMode": request.runtime_mode,
                    "error": format!("Failed to launch detached Rust job runner: {error}"),
                });
                self.store.write_status(&request.id, &failed).await.ok();
                return Err(error);
            }
        };
        Ok(JobStartSummary {
            id: request.id,
            status: "queued".to_owned(),
            runner_pid,
            job_dir: paths.dir.to_string_lossy().into_owned(),
            mode: request.mode.as_str().to_owned(),
            resource_class: request.resource_class.as_str().to_owned(),
        })
    }
}

#[must_use]
pub fn job_store_config(config: &Config) -> JobStoreConfig {
    JobStoreConfig {
        root: config.jobs_root.clone(),
        log_max_bytes: config.job_log_max_bytes,
        log_rotations: config.job_log_rotations,
        orphan_stale: Duration::from_millis(config.job_orphan_stale_ms),
        retention: Duration::from_secs(config.job_retention_hours.saturating_mul(3600)),
        max_wait: Duration::from_secs_f64(config.max_wait_seconds.max(0.1)),
        max_store_bytes: config.job_store_max_bytes,
        max_terminal_jobs: config.job_store_max_terminal_jobs,
    }
}

#[must_use]
pub fn infer_resource_class(text: &str, requested: &str) -> ResourceClass {
    match requested.trim().to_ascii_lowercase().as_str() {
        "watch" => return ResourceClass::Watch,
        "light" => return ResourceClass::Light,
        "heavy" => return ResourceClass::Heavy,
        _ => {}
    }
    let text = text.to_ascii_lowercase();
    let heavy_markers = [
        "playwright",
        "selenium",
        "gradle",
        "ninja",
        "cmake --build",
        "cargo build",
        "cargo test",
        "zig build",
        "npm build",
        "npm run build",
        "pnpm build",
        "pnpm run build",
        "yarn build",
        "bazel",
        "msbuild",
        "dotnet build",
    ];
    if heavy_markers.iter().any(|marker| text.contains(marker))
        || text.split_whitespace().next() == Some("make")
    {
        return ResourceClass::Heavy;
    }
    if text.contains("gh run watch")
        || text.contains("start-sleep")
        || contains_numeric_sleep(&text)
    {
        return ResourceClass::Watch;
    }
    ResourceClass::Light
}

fn contains_numeric_sleep(text: &str) -> bool {
    let parts = text.split_whitespace().collect::<Vec<_>>();
    parts
        .windows(2)
        .any(|pair| pair[0] == "sleep" && pair[1].bytes().all(|byte| byte.is_ascii_digit()))
}

fn spawn_detached_runner(config: &Config, paths: &JobPaths) -> Result<Option<u32>> {
    let executable =
        std::env::current_exe().context("resolve Rust MCP executable for job runner")?;
    let mut command = Command::new(executable);
    command
        .arg("--job-runner")
        .arg(&paths.request)
        .current_dir(&config.project_root)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .kill_on_drop(false);
    configure_detached(&mut command);
    let child = command.spawn().context("spawn detached Rust job runner")?;
    let pid = child.id();
    drop(child);
    Ok(pid)
}

#[cfg(windows)]
fn configure_detached(command: &mut Command) {
    const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    command.creation_flags(CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW);
}

#[cfg(unix)]
fn configure_detached(command: &mut Command) {
    command.process_group(0);
}

#[cfg(not(any(windows, unix)))]
fn configure_detached(_: &mut Command) {}

fn bounded_timeout_ms(timeout: Duration) -> u64 {
    u64::try_from(timeout.as_millis())
        .unwrap_or(u64::MAX)
        .clamp(1_000, 86_400_000)
}

fn new_job_id() -> String {
    let elapsed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let counter = JOB_COUNTER.fetch_add(1, Ordering::Relaxed);
    format!(
        "job-{}-{:x}{:x}{:x}",
        base36(elapsed.as_millis()),
        std::process::id(),
        elapsed.subsec_nanos(),
        counter
    )
}

fn base36(mut value: u128) -> String {
    if value == 0 {
        return "0".to_owned();
    }
    let mut output = Vec::new();
    while value > 0 {
        let digit = u8::try_from(value % 36).unwrap_or(0);
        output.push(if digit < 10 {
            b'0' + digit
        } else {
            b'a' + digit - 10
        });
        value /= 36;
    }
    output.reverse();
    String::from_utf8(output).unwrap_or_else(|_| "0".to_owned())
}

fn utc_now() -> String {
    OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_else(|_| "1970-01-01T00:00:00Z".to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resource_class_inference_matches_production_categories() {
        assert_eq!(
            infer_resource_class("cargo test --all", "auto"),
            ResourceClass::Heavy
        );
        assert_eq!(
            infer_resource_class("gh run watch 123", "auto"),
            ResourceClass::Watch
        );
        assert_eq!(
            infer_resource_class("Start-Sleep 60", "auto"),
            ResourceClass::Watch
        );
        assert_eq!(
            infer_resource_class("sleep 20", "auto"),
            ResourceClass::Watch
        );
        assert_eq!(
            infer_resource_class("git status", "auto"),
            ResourceClass::Light
        );
        assert_eq!(
            infer_resource_class("git status", "heavy"),
            ResourceClass::Heavy
        );
    }

    #[test]
    fn generated_job_ids_fit_persisted_contract() {
        for _ in 0..128 {
            let id = new_job_id();
            assert!((8..=81).contains(&id.len()));
            assert!(id.bytes().next().unwrap().is_ascii_alphanumeric());
            assert!(
                id.bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
            );
        }
    }

    #[test]
    fn timeout_is_clamped_to_javascript_bounds() {
        assert_eq!(bounded_timeout_ms(Duration::ZERO), 1_000);
        assert_eq!(bounded_timeout_ms(Duration::from_secs(2)), 2_000);
        assert_eq!(bounded_timeout_ms(Duration::from_secs(100_000)), 86_400_000);
    }
}
