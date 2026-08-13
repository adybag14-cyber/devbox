use std::{
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant},
};

use serde_json::Value;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

use super::{RuntimeExecError, RuntimeExecutor, ShellRequest};
use crate::{
    process::{
        OutputChunk, OutputStream, ProcessError, ProcessOptions, ProcessOutput, spawn_process,
    },
    windows_shell,
};

#[derive(Debug, Clone)]
struct PowerShellSpawnRequest {
    args: Vec<String>,
    cwd: PathBuf,
    timeout: Duration,
    max_capture_chars: Option<usize>,
    output_tx: Option<tokio::sync::mpsc::Sender<OutputChunk>>,
    pid_tx: Option<tokio::sync::mpsc::UnboundedSender<u32>>,
}

impl RuntimeExecutor {
    pub(super) async fn windows_admin_state(
        &self,
        cancellation: CancellationToken,
    ) -> Result<bool, RuntimeExecError> {
        if let Some(value) = self.windows_admin_state.get() {
            return Ok(*value);
        }
        let probe_cancel = cancellation.child_token();
        let value = self
            .windows_admin_state
            .get_or_try_init(|| async { self.detect_windows_admin(probe_cancel).await })
            .await?;
        Ok(*value)
    }

    async fn detect_windows_admin(
        &self,
        cancellation: CancellationToken,
    ) -> Result<bool, RuntimeExecError> {
        let output = self
            .spawn_windows_powershell(
                PowerShellSpawnRequest {
                    args: windows_shell::encoded_command_args(windows_shell::admin_check_command()),
                    cwd: self.config.host_default_workdir.clone(),
                    timeout: Duration::from_secs(15),
                    max_capture_chars: Some(16_384),
                    output_tx: None,
                    pid_tx: None,
                },
                cancellation,
            )
            .await?;
        let stdout = windows_shell::clean_output(&output.stdout);
        let value: Value = serde_json::from_str(stdout.trim()).map_err(|error| {
            RuntimeExecError::WindowsAdminProbe(format!(
                "Failed to parse Windows administrator-state probe: {error}"
            ))
        })?;
        value
            .get("isAdmin")
            .and_then(Value::as_bool)
            .ok_or_else(|| {
                RuntimeExecError::WindowsAdminProbe(
                    "Windows administrator-state probe did not return isAdmin.".to_owned(),
                )
            })
    }

    pub(super) async fn run_windows_host_shell(
        &self,
        request: ShellRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let is_admin = self.windows_admin_state(cancellation.child_token()).await?;
        if !is_admin && !self.config.allow_windows_host_exec_uac {
            return Err(RuntimeExecError::WindowsElevationRequired);
        }
        if windows_shell::should_use_script_file(&request.command) || !is_admin {
            return self
                .run_windows_powershell_from_file(request, is_admin, cancellation)
                .await;
        }

        let args = windows_shell::encoded_command_args(&request.command);
        match self
            .spawn_windows_powershell(
                PowerShellSpawnRequest {
                    args,
                    cwd: request.working_dir.clone(),
                    timeout: request.timeout,
                    max_capture_chars: request.max_capture_chars,
                    output_tx: request.output_tx.clone(),
                    pid_tx: request.pid_tx.clone(),
                },
                cancellation.child_token(),
            )
            .await
        {
            Ok(output) => Ok(clean_output(output)),
            Err(RuntimeExecError::Process(error)) if is_command_too_long_error(&error) => {
                self.run_windows_powershell_from_file(request, true, cancellation)
                    .await
            }
            Err(RuntimeExecError::Process(error)) => {
                Err(RuntimeExecError::Process(clean_process_error(error)))
            }
            Err(error) => Err(error),
        }
    }

    async fn run_windows_powershell_from_file(
        &self,
        request: ShellRequest,
        is_admin: bool,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let temp_dir =
            std::env::temp_dir().join(format!("devbox-mcp-powershell-{}", Uuid::new_v4()));
        tokio::fs::create_dir_all(&temp_dir)
            .await
            .map_err(|error| RuntimeExecError::WindowsAdminProbe(error.to_string()))?;
        let script_path = temp_dir.join("command.ps1");
        let script = windows_shell::with_quiet_prelude(&request.command);
        let result = async {
            tokio::fs::write(&script_path, script.as_bytes())
                .await
                .map_err(|error| RuntimeExecError::WindowsAdminProbe(error.to_string()))?;
            if is_admin {
                self.spawn_windows_powershell(
                    PowerShellSpawnRequest {
                        args: windows_shell::file_args(&script_path),
                        cwd: request.working_dir,
                        timeout: request.timeout,
                        max_capture_chars: request.max_capture_chars,
                        output_tx: request.output_tx,
                        pid_tx: request.pid_tx,
                    },
                    cancellation,
                )
                .await
                .map(clean_output)
                .map_err(clean_runtime_process_error)
            } else {
                self.run_uac_elevated_script(request, &script_path, &temp_dir, cancellation)
                    .await
            }
        }
        .await;
        tokio::fs::remove_dir_all(&temp_dir).await.ok();
        result
    }

    async fn run_uac_elevated_script(
        &self,
        request: ShellRequest,
        script_path: &std::path::Path,
        temp_dir: &std::path::Path,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let stdout_path = temp_dir.join("stdout.txt");
        let stderr_path = temp_dir.join("stderr.txt");
        let exit_code_path = temp_dir.join("exitcode.txt");
        let elevated_pid_path = temp_dir.join("elevated-pid.txt");
        let timeout_ms = u64::try_from(request.timeout.as_millis())
            .unwrap_or(u64::MAX)
            .max(1);
        let launcher = windows_shell::elevated_launcher(
            &self.config.power_shell_exe,
            script_path,
            &request.working_dir,
            &stdout_path,
            &stderr_path,
            &exit_code_path,
            &elevated_pid_path,
            timeout_ms,
        );
        let started = Instant::now();
        let launcher_result = self
            .spawn_windows_powershell(
                PowerShellSpawnRequest {
                    args: windows_shell::encoded_command_args(&launcher),
                    cwd: request.working_dir,
                    timeout: request.timeout.saturating_add(Duration::from_secs(15)),
                    max_capture_chars: request.max_capture_chars,
                    output_tx: None,
                    pid_tx: request.pid_tx,
                },
                cancellation,
            )
            .await;
        if launcher_result.is_err() {
            terminate_reported_elevated_process_tree(&elevated_pid_path).await;
        }
        let launcher_output = launcher_result.map_err(clean_runtime_process_error)?;
        let stdout = read_text_file_or_empty(&stdout_path).await;
        let stderr = read_text_file_or_empty(&stderr_path).await;
        let exit_code_text = read_text_file_or_empty(&exit_code_path).await;
        let stdout = windows_shell::clean_output(&stdout);
        let stderr = windows_shell::clean_output(&stderr);
        emit_buffered_output(request.output_tx.as_ref(), OutputStream::Stdout, &stdout).await;
        emit_buffered_output(request.output_tx.as_ref(), OutputStream::Stderr, &stderr).await;
        let Ok(exit_code) = exit_code_text.trim().parse::<i32>() else {
            return Err(RuntimeExecError::Process(ProcessError {
                message: "The elevated PowerShell command did not report an exit code.".to_owned(),
                exit_code: None,
                stdout: stdout.into_boxed_str(),
                stderr: stderr.into_boxed_str(),
                file: self.config.power_shell_exe.clone().into_boxed_str(),
                args: windows_shell::file_args(script_path).into_boxed_slice(),
                timed_out: false,
                aborted: false,
                signal: None,
                elapsed_ms: elapsed_ms(started.elapsed()),
            }));
        };
        if exit_code != 0 {
            return Err(RuntimeExecError::Process(ProcessError {
                message: stderr
                    .trim()
                    .to_owned()
                    .or_else_nonempty(stdout.trim(), "Windows PowerShell command failed."),
                exit_code: Some(exit_code),
                stdout: stdout.into_boxed_str(),
                stderr: stderr.into_boxed_str(),
                file: self.config.power_shell_exe.clone().into_boxed_str(),
                args: windows_shell::file_args(script_path).into_boxed_slice(),
                timed_out: false,
                aborted: false,
                signal: None,
                elapsed_ms: elapsed_ms(started.elapsed()),
            }));
        }
        Ok(ProcessOutput {
            stdout_original_chars: stdout.chars().count(),
            stderr_original_chars: stderr.chars().count(),
            stdout_capture_truncated: false,
            stderr_capture_truncated: false,
            stdout,
            stderr,
            exit_code,
            pid: launcher_output.pid,
            elapsed_ms: elapsed_ms(started.elapsed()),
        })
    }

    async fn spawn_windows_powershell(
        &self,
        request: PowerShellSpawnRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessOutput, RuntimeExecError> {
        let mut candidates = vec![self.config.power_shell_exe.clone()];
        if !self.config.power_shell_fallback_exe.is_empty()
            && self.config.power_shell_fallback_exe != self.config.power_shell_exe
        {
            candidates.push(self.config.power_shell_fallback_exe.clone());
        }
        let mut last_error = None;
        let last_index = candidates.len().saturating_sub(1);
        for (index, executable) in candidates.into_iter().enumerate() {
            match spawn_process(
                &executable,
                &request.args,
                ProcessOptions {
                    cwd: Some(request.cwd.clone()),
                    timeout: Some(request.timeout),
                    max_capture_chars: request.max_capture_chars,
                    output_tx: request.output_tx.clone(),
                    pid_tx: request.pid_tx.clone(),
                    ..ProcessOptions::default()
                },
                cancellation.child_token(),
            )
            .await
            {
                Ok(output) => return Ok(output),
                Err(error) if index < last_index && is_powershell_launch_failure(&error) => {
                    tracing::warn!(
                        %executable,
                        fallback = %self.config.power_shell_fallback_exe,
                        %error,
                        "PowerShell launch failed; retrying fallback executable"
                    );
                    last_error = Some(error);
                }
                Err(error) => return Err(RuntimeExecError::Process(error)),
            }
        }
        Err(RuntimeExecError::Process(last_error.unwrap_or_else(|| {
            ProcessError {
                message: "No usable PowerShell executable is configured.".to_owned(),
                exit_code: None,
                stdout: Box::default(),
                stderr: Box::default(),
                file: self.config.power_shell_exe.clone().into_boxed_str(),
                args: request.args.into_boxed_slice(),
                timed_out: false,
                aborted: false,
                signal: None,
                elapsed_ms: 0,
            }
        })))
    }
}

trait NonEmptyFallback {
    fn or_else_nonempty(self, second: &str, fallback: &str) -> String;
}

impl NonEmptyFallback for String {
    fn or_else_nonempty(self, second: &str, fallback: &str) -> String {
        if !self.is_empty() {
            self
        } else if !second.is_empty() {
            second.to_owned()
        } else {
            fallback.to_owned()
        }
    }
}

fn clean_output(mut output: ProcessOutput) -> ProcessOutput {
    output.stdout = windows_shell::clean_output(&output.stdout);
    output.stderr = windows_shell::clean_output(&output.stderr);
    output
}

fn clean_process_error(mut error: ProcessError) -> ProcessError {
    error.stdout = windows_shell::clean_output(&error.stdout).into_boxed_str();
    error.stderr = windows_shell::clean_output(&error.stderr).into_boxed_str();
    let cleaned_message = windows_shell::clean_output(&error.message);
    error.message.clear();
    error.message.push_str(cleaned_message.trim());
    if error.message.is_empty() {
        error.message = error
            .stderr
            .trim()
            .to_owned()
            .or_else_nonempty("", "Windows PowerShell command failed.");
    }
    error
}

fn clean_runtime_process_error(error: RuntimeExecError) -> RuntimeExecError {
    match error {
        RuntimeExecError::Process(error) => RuntimeExecError::Process(clean_process_error(error)),
        other => other,
    }
}

fn is_powershell_launch_failure(error: &ProcessError) -> bool {
    if error.exit_code.is_some() || error.timed_out || error.aborted {
        return false;
    }
    let message = error.message.to_ascii_lowercase();
    [
        "os error 2",
        "no such file or directory",
        "the system cannot find the file specified",
        "executable file not found",
        "program not found",
    ]
    .iter()
    .any(|needle| message.contains(needle))
}

fn is_command_too_long_error(error: &ProcessError) -> bool {
    [error.message.as_str(), &error.stdout, &error.stderr]
        .into_iter()
        .any(|value| {
            let value = value.to_ascii_lowercase();
            value.contains("enametoolong")
                || value.contains("filename or extension is too long")
                || value.contains("command line is too long")
        })
}

async fn terminate_reported_elevated_process_tree(pid_path: &std::path::Path) {
    const PID_DISCOVERY_ATTEMPTS: usize = 20;
    for attempt in 0..PID_DISCOVERY_ATTEMPTS {
        if let Ok(value) = tokio::fs::read_to_string(pid_path).await
            && let Ok(pid) = value.trim().parse::<u32>()
            && pid > 0
        {
            crate::process::terminate_process_tree(pid).await;
            return;
        }
        if attempt + 1 < PID_DISCOVERY_ATTEMPTS {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
    }
}

async fn read_text_file_or_empty(path: &std::path::Path) -> String {
    tokio::fs::read_to_string(path).await.unwrap_or_default()
}

async fn emit_buffered_output(
    output_tx: Option<&tokio::sync::mpsc::Sender<OutputChunk>>,
    stream: OutputStream,
    value: &str,
) {
    let Some(output_tx) = output_tx else {
        return;
    };
    if value.is_empty() {
        return;
    }
    let _ = output_tx
        .send(OutputChunk {
            stream,
            bytes: Arc::<[u8]>::from(value.as_bytes()),
        })
        .await;
}

fn elapsed_ms(duration: Duration) -> u64 {
    u64::try_from(duration.as_millis()).unwrap_or(u64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn process_error(message: &str) -> ProcessError {
        ProcessError {
            message: message.to_owned(),
            exit_code: None,
            stdout: Box::default(),
            stderr: Box::default(),
            file: "pwsh.exe".into(),
            args: Box::default(),
            timed_out: false,
            aborted: false,
            signal: None,
            elapsed_ms: 1,
        }
    }

    #[test]
    fn powershell_fallback_retries_only_explicit_launch_failures() {
        assert!(is_powershell_launch_failure(&process_error(
            "The system cannot find the file specified. (os error 2)"
        )));
        assert!(!is_powershell_launch_failure(&process_error(
            "Command process disappeared without an exit status."
        )));
    }
}
