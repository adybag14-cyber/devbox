use std::{path::PathBuf, sync::Arc, time::Duration};

use anyhow::{Result, bail};
use serde::Serialize;
use tokio_util::sync::CancellationToken;

use crate::{
    Config, RuntimeMode,
    lifecycle::{LifecycleAction, LifecycleService},
    runtime::{ProgramRequest, RuntimeExecError, RuntimeExecutor, ShellRequest},
};

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct GithubAuthStatus {
    pub status_summary: String,
    pub user_name: String,
    pub user_email: String,
}

#[derive(Debug, Clone)]
pub struct GithubAuthSyncResult {
    pub status: GithubAuthStatus,
    pub host_user_name: String,
    pub host_user_email: String,
}

#[derive(Debug, Clone)]
struct HostGithubContext {
    token: String,
    status: GithubAuthStatus,
}

#[derive(Debug, Clone)]
pub struct GithubAuthService {
    config: Arc<Config>,
    runtime: Arc<RuntimeExecutor>,
    lifecycle: Arc<LifecycleService>,
}

impl GithubAuthService {
    #[must_use]
    pub fn new(
        config: Arc<Config>,
        runtime: Arc<RuntimeExecutor>,
        lifecycle: Arc<LifecycleService>,
    ) -> Self {
        Self {
            config,
            runtime,
            lifecycle,
        }
    }

    /// Return GitHub CLI authentication and git identity for the selected Devbox runtime.
    ///
    /// The host token is never included in this result.
    ///
    /// # Errors
    /// Returns host/runtime execution errors or a missing GitHub CLI login.
    pub async fn status(&self, cancellation: CancellationToken) -> Result<GithubAuthStatus> {
        match self.config.runtime_mode {
            RuntimeMode::Host => Ok(self.host_context(cancellation).await?.status),
            RuntimeMode::Docker => self.selected_runtime_status(cancellation).await,
        }
    }

    /// Copy the existing host GitHub CLI token and global git identity into the selected runtime.
    ///
    /// The token is passed only through stdin to `gh auth login --with-token` and is never
    /// returned from this method.
    ///
    /// # Errors
    /// Returns when host auth is missing or any login/setup/config command fails.
    pub async fn sync_from_host(
        &self,
        cancellation: CancellationToken,
    ) -> Result<GithubAuthSyncResult> {
        let host = self.host_context(cancellation.child_token()).await?;
        self.ensure_selected_runtime(cancellation.child_token())
            .await?;
        self.run_selected_program(
            "gh",
            vec![
                "auth".to_owned(),
                "login".to_owned(),
                "--hostname".to_owned(),
                "github.com".to_owned(),
                "--with-token".to_owned(),
            ],
            Some(format!("{}\n", host.token).into_bytes()),
            Duration::from_secs(20),
            cancellation.child_token(),
        )
        .await?;
        self.run_selected_program(
            "gh",
            vec![
                "auth".to_owned(),
                "setup-git".to_owned(),
                "--hostname".to_owned(),
                "github.com".to_owned(),
            ],
            None,
            Duration::from_secs(15),
            cancellation.child_token(),
        )
        .await?;
        if !host.status.user_name.is_empty() {
            self.run_selected_program(
                "git",
                vec![
                    "config".to_owned(),
                    "--global".to_owned(),
                    "user.name".to_owned(),
                    host.status.user_name.clone(),
                ],
                None,
                Duration::from_secs(5),
                cancellation.child_token(),
            )
            .await?;
        }
        if !host.status.user_email.is_empty() {
            self.run_selected_program(
                "git",
                vec![
                    "config".to_owned(),
                    "--global".to_owned(),
                    "user.email".to_owned(),
                    host.status.user_email.clone(),
                ],
                None,
                Duration::from_secs(5),
                cancellation.child_token(),
            )
            .await?;
        }
        let status = self.selected_runtime_status(cancellation).await?;
        Ok(GithubAuthSyncResult {
            status,
            host_user_name: host.status.user_name,
            host_user_email: host.status.user_email,
        })
    }

    async fn host_context(&self, cancellation: CancellationToken) -> Result<HostGithubContext> {
        if !self.config.host_exec_enabled {
            bail!("Host execution is disabled.");
        }
        let status = self
            .run_host_program(
                "gh",
                vec![
                    "auth".to_owned(),
                    "status".to_owned(),
                    "--hostname".to_owned(),
                    "github.com".to_owned(),
                ],
                None,
                Duration::from_secs(15),
                cancellation.child_token(),
            )
            .await?;
        let token = self
            .run_host_program(
                "gh",
                vec![
                    "auth".to_owned(),
                    "token".to_owned(),
                    "--hostname".to_owned(),
                    "github.com".to_owned(),
                ],
                None,
                Duration::from_secs(15),
                cancellation.child_token(),
            )
            .await?
            .stdout
            .trim()
            .to_owned();
        if token.is_empty() {
            bail!("Host GitHub CLI did not return a token.");
        }
        let user_name = self
            .try_host_git_config("user.name", cancellation.child_token())
            .await?;
        let user_email = self.try_host_git_config("user.email", cancellation).await?;
        Ok(HostGithubContext {
            token,
            status: GithubAuthStatus {
                status_summary: joined_status(&status.stdout, &status.stderr),
                user_name,
                user_email,
            },
        })
    }

    async fn selected_runtime_status(
        &self,
        cancellation: CancellationToken,
    ) -> Result<GithubAuthStatus> {
        self.ensure_selected_runtime(cancellation.child_token())
            .await?;
        let status = self
            .run_selected_program(
                "gh",
                vec![
                    "auth".to_owned(),
                    "status".to_owned(),
                    "--hostname".to_owned(),
                    "github.com".to_owned(),
                ],
                None,
                Duration::from_secs(15),
                cancellation.child_token(),
            )
            .await?;
        let user_name = self
            .selected_git_identity("user.name", cancellation.child_token())
            .await?;
        let user_email = self
            .selected_git_identity("user.email", cancellation)
            .await?;
        Ok(GithubAuthStatus {
            status_summary: joined_status(&status.stdout, &status.stderr),
            user_name,
            user_email,
        })
    }

    async fn ensure_selected_runtime(&self, cancellation: CancellationToken) -> Result<()> {
        if self.config.runtime_mode == RuntimeMode::Docker {
            self.lifecycle
                .control(LifecycleAction::Start, cancellation)
                .await?;
        }
        Ok(())
    }

    async fn selected_git_identity(
        &self,
        key: &str,
        cancellation: CancellationToken,
    ) -> Result<String> {
        if self.config.runtime_mode == RuntimeMode::Docker {
            let command = format!("git config --global --get {key} || true");
            return Ok(self
                .runtime
                .run_shell(
                    ShellRequest {
                        command,
                        working_dir: self.config.devbox_workspace_path.clone(),
                        timeout: Duration::from_secs(5),
                        user: self.config.devbox_default_user.clone(),
                        max_capture_chars: Some(8_192),
                        output_tx: None,
                        pid_tx: None,
                    },
                    cancellation,
                )
                .await?
                .stdout
                .trim()
                .to_owned());
        }
        self.try_host_git_config(key, cancellation).await
    }

    async fn try_host_git_config(
        &self,
        key: &str,
        cancellation: CancellationToken,
    ) -> Result<String> {
        match self
            .run_host_program(
                "git",
                vec!["config".to_owned(), "--global".to_owned(), key.to_owned()],
                None,
                Duration::from_secs(5),
                cancellation,
            )
            .await
        {
            Ok(output) => Ok(output.stdout.trim().to_owned()),
            Err(error)
                if error
                    .downcast_ref::<crate::process::ProcessError>()
                    .is_some() =>
            {
                Ok(String::new())
            }
            Err(error) => Err(error),
        }
    }

    async fn run_host_program(
        &self,
        program: &str,
        args: Vec<String>,
        input: Option<Vec<u8>>,
        timeout: Duration,
        cancellation: CancellationToken,
    ) -> Result<crate::process::ProcessOutput> {
        self.runtime
            .run_host_program_only(
                ProgramRequest {
                    program: program.to_owned(),
                    args,
                    input,
                    working_dir: self.config.host_default_workdir.clone(),
                    timeout,
                    user: String::new(),
                    max_capture_chars: Some(65_536),
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await
            .map_err(runtime_error)
    }

    async fn run_selected_program(
        &self,
        program: &str,
        args: Vec<String>,
        input: Option<Vec<u8>>,
        timeout: Duration,
        cancellation: CancellationToken,
    ) -> Result<crate::process::ProcessOutput> {
        self.runtime
            .run_program(
                ProgramRequest {
                    program: program.to_owned(),
                    args,
                    input,
                    working_dir: if self.config.runtime_mode == RuntimeMode::Host {
                        PathBuf::from(&self.config.host_default_workdir)
                    } else {
                        self.config.devbox_workspace_path.clone()
                    },
                    timeout,
                    user: self.config.devbox_default_user.clone(),
                    max_capture_chars: Some(65_536),
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await
            .map_err(runtime_error)
    }
}

fn runtime_error(error: RuntimeExecError) -> anyhow::Error {
    match error {
        RuntimeExecError::Process(process) => anyhow::Error::new(process),
        other => anyhow::Error::new(other),
    }
}

fn joined_status(stdout: &str, stderr: &str) -> String {
    format!("{stdout}{stderr}").trim().to_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn public_status_serialization_has_no_token_field() {
        let status = GithubAuthStatus {
            status_summary: "github.com logged in".to_owned(),
            user_name: "Ada".to_owned(),
            user_email: "ada@example.test".to_owned(),
        };
        let value = serde_json::to_value(status).expect("serialize status");
        assert_eq!(value["statusSummary"], "github.com logged in");
        assert_eq!(value["userName"], "Ada");
        assert!(value.get("token").is_none());
    }

    #[test]
    fn status_summary_matches_javascript_stream_joining() {
        assert_eq!(joined_status("alpha\n", "beta\n"), "alpha\nbeta");
        assert_eq!(joined_status("alpha", ""), "alpha");
    }
}
