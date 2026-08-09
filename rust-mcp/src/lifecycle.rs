use std::{
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use chrono::{SecondsFormat, Utc};
use serde_json::{Value, json};
use tokio::{fs, sync::Mutex};
use tokio_util::sync::CancellationToken;

use crate::{
    Config, RuntimeMode,
    process::{ProcessError, ProcessOptions, ProcessOutput, spawn_process},
};

static UNIQUE_COUNTER: AtomicU64 = AtomicU64::new(0);
const DOCKER_CAPTURE_CHARS: usize = 262_144;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LifecycleAction {
    Start,
    Stop,
    Restart,
    Recreate,
}

impl LifecycleAction {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Start => "start",
            Self::Stop => "stop",
            Self::Restart => "restart",
            Self::Recreate => "recreate",
        }
    }
}

#[derive(Debug, Clone)]
pub struct LifecycleService {
    config: Arc<Config>,
    gate: Arc<Mutex<()>>,
}

impl LifecycleService {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        Self {
            config,
            gate: Arc::new(Mutex::new(())),
        }
    }

    /// Apply one Devbox lifecycle action with the same host-vs-Docker semantics as the JS MCP.
    ///
    /// Host mode never terminates the MCP process itself. Docker mode serializes all lifecycle
    /// mutations, creates the configured container when required, and rolls recreate back when
    /// replacement creation or legacy `/tmp` migration fails.
    ///
    /// # Errors
    /// Returns filesystem, Docker, cancellation, JSON parsing, or rollback errors.
    /// Inspect the current selected runtime without mutating it.
    ///
    /// # Errors
    /// Returns Docker inspection or cancellation failures.
    pub async fn status(&self, cancellation: CancellationToken) -> Result<Value> {
        match self.config.runtime_mode {
            RuntimeMode::Host => Ok(host_runtime_info(&self.config)),
            RuntimeMode::Docker => self.inspect(cancellation).await,
        }
    }

    /// Persist the Guardian desired-running state using the same file contract as the JS server.
    ///
    /// # Errors
    /// Returns filesystem errors while creating or atomically replacing the state file.
    pub async fn set_guardian_desired_state(&self, should_run: bool, source: &str) -> Result<()> {
        let path = self
            .config
            .project_root
            .join("run")
            .join("guardian.desired-state.json");
        let parent = path
            .parent()
            .context("Guardian desired-state path has no parent")?;
        fs::create_dir_all(parent).await?;
        let temporary = path.with_extension(format!("{}.tmp", unique_suffix()));
        let value = json!({
            "ShouldRun": should_run,
            "UpdatedAtUtc": Utc::now().to_rfc3339_opts(SecondsFormat::Millis, true),
            "Source": source,
        });
        fs::write(
            &temporary,
            format!(
                "{}
",
                serde_json::to_string_pretty(&value)?
            ),
        )
        .await?;
        if fs::rename(&temporary, &path).await.is_err() {
            fs::remove_file(&path).await.ok();
            fs::rename(&temporary, &path).await?;
        }
        Ok(())
    }

    /// Apply one start/stop/restart/recreate mutation under the shared lifecycle gate.
    ///
    /// # Errors
    /// Returns host filesystem, Docker process, cancellation, or recreation failures.
    pub async fn control(
        &self,
        action: LifecycleAction,
        cancellation: CancellationToken,
    ) -> Result<Value> {
        let _guard = self.gate.lock().await;
        match self.config.runtime_mode {
            RuntimeMode::Host => self.control_host(action).await,
            RuntimeMode::Docker => self.control_docker(action, cancellation).await,
        }
    }

    async fn control_host(&self, action: LifecycleAction) -> Result<Value> {
        if action == LifecycleAction::Start {
            fs::create_dir_all(&self.config.host_workspace_path)
                .await
                .with_context(|| {
                    format!(
                        "create host workspace {}",
                        self.config.host_workspace_path.display()
                    )
                })?;
        }
        let mut info = host_runtime_info(&self.config);
        if action != LifecycleAction::Start {
            info["controlAction"] = Value::String(action.as_str().to_owned());
            info["controlMessage"] = Value::String(format!(
                "Host mode runs inside the current server process. Use the devbox launcher command to {} the service itself.",
                action.as_str()
            ));
        }
        Ok(info)
    }

    async fn control_docker(
        &self,
        action: LifecycleAction,
        cancellation: CancellationToken,
    ) -> Result<Value> {
        match action {
            LifecycleAction::Start => self.ensure_running(cancellation).await,
            LifecycleAction::Stop => self.stop(cancellation).await,
            LifecycleAction::Restart => self.restart(cancellation).await,
            LifecycleAction::Recreate => self.recreate(cancellation).await,
        }
    }

    async fn ensure_running(&self, cancellation: CancellationToken) -> Result<Value> {
        let info = self.inspect(cancellation.child_token()).await?;
        if info["exists"].as_bool() != Some(true) {
            if !self.config.devbox_auto_start {
                bail!(
                    "Devbox container \"{}\" does not exist and DEVBOX_AUTO_START is disabled.",
                    self.config.devbox_container_name
                );
            }
            return self.create_container(cancellation).await;
        }
        if info["running"].as_bool() != Some(true) {
            self.run_docker(
                vec![
                    "start".to_owned(),
                    self.config.devbox_container_name.clone(),
                ],
                cancellation.child_token(),
            )
            .await?;
        }
        self.inspect(cancellation).await
    }

    async fn stop(&self, cancellation: CancellationToken) -> Result<Value> {
        let info = self.inspect(cancellation.child_token()).await?;
        if info["exists"].as_bool() != Some(true) {
            return Ok(info);
        }
        if info["running"].as_bool() == Some(true) {
            self.run_docker(
                vec!["stop".to_owned(), self.config.devbox_container_name.clone()],
                cancellation.child_token(),
            )
            .await?;
        }
        self.inspect(cancellation).await
    }

    async fn restart(&self, cancellation: CancellationToken) -> Result<Value> {
        let info = self.inspect(cancellation.child_token()).await?;
        if info["exists"].as_bool() != Some(true) {
            return self.create_container(cancellation).await;
        }
        self.run_docker(
            vec![
                "restart".to_owned(),
                self.config.devbox_container_name.clone(),
            ],
            cancellation.child_token(),
        )
        .await?;
        self.inspect(cancellation).await
    }

    async fn recreate(&self, cancellation: CancellationToken) -> Result<Value> {
        let info = self.inspect(cancellation.child_token()).await?;
        let exists = info["exists"].as_bool() == Some(true);
        let needs_tmp_migration =
            exists && !has_managed_tmp_volume(&info, &self.config.devbox_tmp_volume_name);
        let retired = exists.then(|| retired_container_name(&self.config.devbox_container_name));

        if let Some(retired_name) = retired.as_deref() {
            self.run_docker(
                vec![
                    "rename".to_owned(),
                    self.config.devbox_container_name.clone(),
                    retired_name.to_owned(),
                ],
                cancellation.child_token(),
            )
            .await?;
        }

        let replacement = self.create_container(cancellation.child_token()).await;
        match replacement {
            Ok(recreated) => {
                if let Some(retired_name) = retired.as_deref()
                    && needs_tmp_migration
                    && let Err(error) = self
                        .copy_legacy_tmp(retired_name, cancellation.child_token())
                        .await
                {
                    return self
                        .rollback_recreate(retired_name, error, cancellation)
                        .await;
                }
                if let Some(retired_name) = retired {
                    self.queue_retired_cleanup(retired_name);
                }
                Ok(recreated)
            }
            Err(error) => {
                if let Some(retired_name) = retired.as_deref() {
                    self.rollback_recreate(retired_name, error, cancellation)
                        .await
                } else {
                    Err(error)
                }
            }
        }
    }

    async fn rollback_recreate(
        &self,
        retired_name: &str,
        original: anyhow::Error,
        cancellation: CancellationToken,
    ) -> Result<Value> {
        self.remove_container_if_present(
            &self.config.devbox_container_name,
            cancellation.child_token(),
        )
        .await?;
        let restore = self
            .run_docker(
                vec![
                    "rename".to_owned(),
                    retired_name.to_owned(),
                    self.config.devbox_container_name.clone(),
                ],
                cancellation,
            )
            .await;
        match restore {
            Ok(_) => Err(original),
            Err(restore_error) => bail!("{original} Rollback also failed: {restore_error}"),
        }
    }

    async fn create_container(&self, cancellation: CancellationToken) -> Result<Value> {
        self.run_docker(
            create_container_args(&self.config),
            cancellation.child_token(),
        )
        .await?;
        self.inspect(cancellation).await
    }

    async fn inspect(&self, cancellation: CancellationToken) -> Result<Value> {
        let args = vec![
            "inspect".to_owned(),
            "--type".to_owned(),
            "container".to_owned(),
            self.config.devbox_container_name.clone(),
            "--format".to_owned(),
            "{{json .}}".to_owned(),
        ];
        match self.run_docker(args, cancellation).await {
            Ok(output) => parse_docker_info(&output.stdout, &self.config.devbox_container_name),
            Err(error) if is_missing_container_error(&error) => {
                Ok(missing_container_info(&self.config.devbox_container_name))
            }
            Err(error) => Err(error),
        }
    }

    async fn copy_legacy_tmp(
        &self,
        retired_name: &str,
        cancellation: CancellationToken,
    ) -> Result<()> {
        let staging_root = std::env::temp_dir().join(format!(
            "docker-chatgpt-devbox-rust-tmp-{}",
            unique_suffix()
        ));
        let staging_path = staging_root.join("payload");
        fs::create_dir_all(&staging_root).await?;
        let result = async {
            self.run_docker(
                vec![
                    "cp".to_owned(),
                    format!("{retired_name}:/tmp"),
                    staging_path.to_string_lossy().into_owned(),
                ],
                cancellation.child_token(),
            )
            .await?;
            let source_contents = format!(
                "{}{}.",
                staging_path.to_string_lossy(),
                std::path::MAIN_SEPARATOR
            );
            self.run_docker(
                vec![
                    "cp".to_owned(),
                    source_contents,
                    format!("{}:/tmp", self.config.devbox_container_name),
                ],
                cancellation,
            )
            .await?;
            Ok::<(), anyhow::Error>(())
        }
        .await;
        fs::remove_dir_all(&staging_root).await.ok();
        result
    }

    async fn remove_container_if_present(
        &self,
        name: &str,
        cancellation: CancellationToken,
    ) -> Result<()> {
        match self
            .run_docker(
                vec!["rm".to_owned(), "-f".to_owned(), name.to_owned()],
                cancellation,
            )
            .await
        {
            Ok(_) => Ok(()),
            Err(error) if is_missing_container_error(&error) => Ok(()),
            Err(error) => Err(error),
        }
    }

    fn queue_retired_cleanup(&self, retired_name: String) {
        let delay = Duration::from_millis(self.config.devbox_retired_container_grace_ms);
        let docker_timeout = Duration::from_millis(self.config.docker_command_timeout_ms);
        tokio::spawn(async move {
            if !delay.is_zero() {
                tokio::time::sleep(delay).await;
            }
            let _ = spawn_process(
                "docker",
                &["rm".to_owned(), "-f".to_owned(), retired_name],
                ProcessOptions {
                    timeout: Some(docker_timeout),
                    max_capture_chars: Some(DOCKER_CAPTURE_CHARS),
                    ..ProcessOptions::default()
                },
                CancellationToken::new(),
            )
            .await;
        });
    }

    async fn run_docker(
        &self,
        args: Vec<String>,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput> {
        spawn_process(
            "docker",
            &args,
            ProcessOptions {
                timeout: Some(Duration::from_millis(self.config.docker_command_timeout_ms)),
                max_capture_chars: Some(DOCKER_CAPTURE_CHARS),
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await
        .map_err(anyhow::Error::new)
    }
}

fn host_runtime_info(config: &Config) -> Value {
    json!({
        "mode": "host",
        "exists": true,
        "running": true,
        "status": "ready",
        "name": format!("{}-host-runtime", config.platform.id),
        "workspacePath": config.host_workspace_path,
        "platform": config.platform.id,
        "hostDefaultWorkdir": config.host_default_workdir,
        "hostShell": config.host_shell,
        "hostShellFallback": host_shell_fallback(config),
    })
}

fn host_shell_fallback(config: &Config) -> Option<&str> {
    if !config.platform.is_windows
        || std::env::var("HOST_SHELL")
            .ok()
            .is_some_and(|value| !value.trim().is_empty())
        || config.power_shell_fallback_exe.trim().is_empty()
    {
        None
    } else {
        Some(config.power_shell_fallback_exe.as_str())
    }
}

fn create_container_args(config: &Config) -> Vec<String> {
    vec![
        "run".to_owned(),
        "-d".to_owned(),
        "--name".to_owned(),
        config.devbox_container_name.clone(),
        "--init".to_owned(),
        "-w".to_owned(),
        config.devbox_workspace_path.to_string_lossy().into_owned(),
        "-v".to_owned(),
        format!(
            "{}:{}",
            config.host_workspace_path.to_string_lossy(),
            config.devbox_workspace_path.to_string_lossy()
        ),
        "-v".to_owned(),
        format!("{}:/tmp", config.devbox_tmp_volume_name),
        config.devbox_image_name.clone(),
        "sleep".to_owned(),
        "infinity".to_owned(),
    ]
}

fn parse_docker_info(stdout: &str, fallback_name: &str) -> Result<Value> {
    let data: Value = serde_json::from_str(stdout.trim()).context("parse Docker inspect JSON")?;
    Ok(json!({
        "exists": true,
        "id": data["Id"].clone(),
        "image": data["Config"]["Image"].clone(),
        "running": data["State"]["Running"].as_bool().unwrap_or(false),
        "status": data["State"]["Status"].as_str().unwrap_or("unknown"),
        "startedAt": data["State"]["StartedAt"].clone(),
        "mounts": data["Mounts"].as_array().cloned().unwrap_or_default(),
        "name": data["Name"]
            .as_str()
            .map_or_else(|| fallback_name.to_owned(), |value| value.trim_start_matches('/').to_owned()),
    }))
}

fn missing_container_info(name: &str) -> Value {
    json!({
        "exists": false,
        "name": name,
        "running": false,
        "status": "missing",
    })
}

fn has_managed_tmp_volume(info: &Value, volume_name: &str) -> bool {
    info["mounts"].as_array().is_some_and(|mounts| {
        mounts.iter().any(|mount| {
            mount["Destination"].as_str() == Some("/tmp")
                && mount["Type"].as_str() == Some("volume")
                && mount["Name"].as_str() == Some(volume_name)
        })
    })
}

fn is_missing_container_error(error: &anyhow::Error) -> bool {
    let mut text = error.to_string();
    if let Some(process) = error.downcast_ref::<ProcessError>() {
        text.push(' ');
        text.push_str(&process.stderr);
        text.push(' ');
        text.push_str(&process.stdout);
    }
    let text = text.to_ascii_lowercase();
    text.contains("no such container") || text.contains("no such object")
}

fn retired_container_name(base: &str) -> String {
    format!("{base}-retired-{}", unique_suffix())
}

fn unique_suffix() -> String {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_millis());
    let counter = UNIQUE_COUNTER.fetch_add(1, Ordering::Relaxed);
    format!("{millis}-{}-{counter}", std::process::id())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn docker_info_parser_matches_javascript_shape() {
        let raw = r#"{
            "Id":"abc",
            "Name":"/box",
            "Config":{"Image":"img:1"},
            "State":{"Running":true,"Status":"running","StartedAt":"2026-01-01T00:00:00Z"},
            "Mounts":[{"Destination":"/tmp","Type":"volume","Name":"box-tmp"}]
        }"#;
        let info = parse_docker_info(raw, "fallback").expect("parse info");
        assert_eq!(info["exists"], true);
        assert_eq!(info["name"], "box");
        assert_eq!(info["running"], true);
        assert_eq!(info["image"], "img:1");
        assert!(has_managed_tmp_volume(&info, "box-tmp"));
        assert!(!has_managed_tmp_volume(&info, "other"));
    }

    #[test]
    fn create_args_match_javascript_order() {
        let root = PathBuf::from("C:/workspace");
        let config = Config {
            project_root: root.clone(),
            host: "127.0.0.1".to_owned(),
            port: 0,
            auth_mode: crate::AuthMode::None,
            runtime_mode: RuntimeMode::Docker,
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
            host_workspace_path: root.clone(),
            devbox_workspace_path: PathBuf::from("/workspace"),
            devbox_container_name: "box".to_owned(),
            devbox_image_name: "img:1".to_owned(),
            devbox_tmp_volume_name: "box-tmp".to_owned(),
            devbox_retired_container_grace_ms: 300_000,
            devbox_auto_start: true,
            devbox_version_cache_ms: 120_000,
            docker_command_timeout_ms: 120_000,
            devbox_default_user: "root".to_owned(),
            host_default_workdir: root.clone(),
            host_shell: "pwsh".to_owned(),
            power_shell_exe: "pwsh".to_owned(),
            power_shell_fallback_exe: "powershell.exe".to_owned(),
            node_exe: "node".to_owned(),
            host_program_allowlist: vec![],
            host_search_backend: crate::config::HostSearchBackend::Auto,
            devbox_program_allowlist: vec![],
            host_exec_enabled: true,
            allow_windows_host_exec_uac: false,
            execution_slot_root: root.join("slots"),
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
        };
        let args = create_container_args(&config);
        assert_eq!(
            args,
            [
                "run",
                "-d",
                "--name",
                "box",
                "--init",
                "-w",
                "/workspace",
                "-v",
                "C:/workspace:/workspace",
                "-v",
                "box-tmp:/tmp",
                "img:1",
                "sleep",
                "infinity",
            ]
        );
    }
}
