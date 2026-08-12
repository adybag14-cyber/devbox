use std::{
    collections::HashMap,
    ffi::OsString,
    path::PathBuf,
    process::{ExitStatus, Stdio},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWriteExt},
    process::{Child, Command},
    sync::mpsc::{Sender, UnboundedSender},
};
use tokio_util::sync::CancellationToken;

pub const MAX_PROCESS_ERROR_MESSAGE_CHARS: usize = 4096;
const POST_EXIT_PIPE_DRAIN: Duration = Duration::from_secs(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutputStream {
    Stdout,
    Stderr,
}

#[derive(Debug, Clone)]
pub struct OutputChunk {
    pub stream: OutputStream,
    pub bytes: Arc<[u8]>,
}

#[derive(Debug, Clone)]
pub struct ProcessOptions {
    pub cwd: Option<PathBuf>,
    pub env: Option<HashMap<OsString, OsString>>,
    pub timeout: Option<Duration>,
    pub termination_grace: Duration,
    pub max_capture_chars: Option<usize>,
    pub input: Option<Vec<u8>>,
    pub output_tx: Option<Sender<OutputChunk>>,
    pub pid_tx: Option<UnboundedSender<u32>>,
}

impl Default for ProcessOptions {
    fn default() -> Self {
        Self {
            cwd: None,
            env: None,
            timeout: None,
            termination_grace: Duration::from_secs(3),
            max_capture_chars: None,
            input: None,
            output_tx: None,
            pid_tx: None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessOutput {
    pub stdout: String,
    pub stderr: String,
    pub exit_code: i32,
    pub pid: u32,
    pub elapsed_ms: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessError {
    pub message: String,
    pub exit_code: Option<i32>,
    pub stdout: Box<str>,
    pub stderr: Box<str>,
    pub file: Box<str>,
    pub args: Box<[String]>,
    pub timed_out: bool,
    pub aborted: bool,
    pub signal: Option<i32>,
    pub elapsed_ms: u64,
}

impl std::fmt::Display for ProcessError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for ProcessError {}

struct CaptureTask {
    task: tokio::task::JoinHandle<()>,
    captured: Arc<Mutex<String>>,
}

struct SpawnedProcess {
    child: Child,
    pid: u32,
    stdout_task: CaptureTask,
    stderr_task: CaptureTask,
    #[cfg(windows)]
    job: Option<crate::windows_job::WindowsJob>,
}

#[derive(Debug)]
enum WaitOutcome {
    Exited(std::io::Result<ExitStatus>),
    Cancelled,
    TimedOut(Duration),
}

#[derive(Debug, Clone, Copy)]
enum ForcedFailure {
    Cancelled,
    TimedOut(Duration),
}

struct ProcessTreeDropGuard {
    pid: u32,
    armed: bool,
    #[cfg(windows)]
    job: Option<crate::windows_job::WindowsJob>,
}

impl ProcessTreeDropGuard {
    #[cfg(windows)]
    const fn new(pid: u32, job: Option<crate::windows_job::WindowsJob>) -> Self {
        Self {
            pid,
            armed: pid > 0,
            job,
        }
    }

    #[cfg(not(windows))]
    const fn new(pid: u32) -> Self {
        Self {
            pid,
            armed: pid > 0,
        }
    }

    const fn disarm(&mut self) {
        self.armed = false;
    }
}

impl Drop for ProcessTreeDropGuard {
    fn drop(&mut self) {
        if self.armed {
            #[cfg(windows)]
            terminate_process_tree_on_drop(self.pid, self.job.as_ref());
            #[cfg(not(windows))]
            terminate_process_tree_on_drop(self.pid);
        }
    }
}

/// Spawn one executable without shell parsing and capture bounded stdout/stderr tails.
///
/// The child is placed in its own process group on Unix. Cancellation and timeout
/// terminate the process tree on Windows and the process group on Unix.
///
/// # Errors
/// Returns a typed [`ProcessError`] for launch errors, non-zero exits, timeout,
/// or caller cancellation.
pub async fn spawn_process(
    file: &str,
    args: &[String],
    options: ProcessOptions,
    cancellation: CancellationToken,
) -> Result<ProcessOutput, ProcessError> {
    let started = Instant::now();
    let mut spawned = start_process(file, args, &options, started)?;
    #[cfg(windows)]
    let mut drop_guard = ProcessTreeDropGuard::new(spawned.pid, spawned.job.take());
    #[cfg(not(windows))]
    let mut drop_guard = ProcessTreeDropGuard::new(spawned.pid);
    let (status, forced_failure) = wait_for_process(
        &mut spawned.child,
        spawned.pid,
        #[cfg(windows)]
        drop_guard.job.as_ref(),
        options.timeout,
        options.termination_grace,
        cancellation,
    )
    .await;
    drop_guard.disarm();

    let drain = status.map(|_| POST_EXIT_PIPE_DRAIN);
    let stdout = join_capture(spawned.stdout_task, drain).await;
    let stderr = join_capture(spawned.stderr_task, drain).await;
    classify_process_result(
        file,
        args,
        spawned.pid,
        started,
        status,
        forced_failure,
        stdout,
        stderr,
    )
}

fn start_process(
    file: &str,
    args: &[String],
    options: &ProcessOptions,
    started: Instant,
) -> Result<SpawnedProcess, ProcessError> {
    let mut command = Command::new(file);
    command.args(args);
    command.stdin(Stdio::piped());
    command.stdout(Stdio::piped());
    command.stderr(Stdio::piped());
    command.kill_on_drop(true);
    if let Some(cwd) = options.cwd.as_ref() {
        command.current_dir(cwd);
    }
    if let Some(env) = options.env.as_ref() {
        command.env_clear();
        command.envs(env);
    }
    configure_process_group(&mut command);

    let mut child = command
        .spawn()
        .map_err(|error| launch_error(file, args, error.to_string(), started))?;
    let pid = child.id().unwrap_or(0);
    #[cfg(windows)]
    let job = if pid > 0 {
        match crate::windows_job::WindowsJob::assign(pid) {
            Ok(job) => Some(job),
            Err(error) => {
                tracing::warn!(pid, %error, "failed to assign Windows child to Job Object; native process-tree fallback will be used");
                None
            }
        }
    } else {
        None
    };
    if pid > 0
        && let Some(pid_tx) = options.pid_tx.as_ref()
    {
        let _ = pid_tx.send(pid);
    }
    start_stdin_writer(child.stdin.take(), options.input.clone());
    let stdout = child.stdout.take().ok_or_else(|| {
        launch_error(
            file,
            args,
            "stdout pipe was unavailable".to_owned(),
            started,
        )
    })?;
    let stderr = child.stderr.take().ok_or_else(|| {
        launch_error(
            file,
            args,
            "stderr pipe was unavailable".to_owned(),
            started,
        )
    })?;
    let stdout_task = start_capture_task(
        stdout,
        OutputStream::Stdout,
        options.max_capture_chars,
        options.output_tx.clone(),
    );
    let stderr_task = start_capture_task(
        stderr,
        OutputStream::Stderr,
        options.max_capture_chars,
        options.output_tx.clone(),
    );
    Ok(SpawnedProcess {
        child,
        pid,
        stdout_task,
        stderr_task,
        #[cfg(windows)]
        job,
    })
}

fn start_stdin_writer(stdin: Option<tokio::process::ChildStdin>, input: Option<Vec<u8>>) {
    let Some(mut stdin) = stdin else {
        return;
    };
    let input = input.unwrap_or_default();
    tokio::spawn(async move {
        if !input.is_empty() {
            let _ = stdin.write_all(&input).await;
        }
        let _ = stdin.shutdown().await;
    });
}

async fn wait_for_process(
    child: &mut Child,
    pid: u32,
    #[cfg(windows)] job: Option<&crate::windows_job::WindowsJob>,
    timeout: Option<Duration>,
    termination_grace: Duration,
    cancellation: CancellationToken,
) -> (Option<ExitStatus>, Option<ForcedFailure>) {
    let wait = child.wait();
    tokio::pin!(wait);
    let outcome = match timeout {
        Some(timeout) => {
            tokio::select! {
                status = &mut wait => WaitOutcome::Exited(status),
                () = cancellation.cancelled() => WaitOutcome::Cancelled,
                () = tokio::time::sleep(timeout) => WaitOutcome::TimedOut(timeout),
            }
        }
        None => {
            tokio::select! {
                status = &mut wait => WaitOutcome::Exited(status),
                () = cancellation.cancelled() => WaitOutcome::Cancelled,
            }
        }
    };

    match outcome {
        WaitOutcome::Exited(status) => (status.ok(), None),
        WaitOutcome::Cancelled => {
            #[cfg(windows)]
            terminate_spawned_tree(pid, job);
            #[cfg(not(windows))]
            terminate_process_tree(pid).await;
            (
                wait_after_termination(&mut wait, termination_grace).await,
                Some(ForcedFailure::Cancelled),
            )
        }
        WaitOutcome::TimedOut(timeout) => {
            #[cfg(windows)]
            terminate_spawned_tree(pid, job);
            #[cfg(not(windows))]
            terminate_process_tree(pid).await;
            (
                wait_after_termination(&mut wait, termination_grace).await,
                Some(ForcedFailure::TimedOut(timeout)),
            )
        }
    }
}

async fn wait_after_termination<F>(wait: &mut F, grace: Duration) -> Option<ExitStatus>
where
    F: FutureExitStatus + Unpin,
{
    tokio::time::timeout(grace, wait)
        .await
        .ok()
        .and_then(Result::ok)
}

trait FutureExitStatus: std::future::Future<Output = std::io::Result<ExitStatus>> {}
impl<T> FutureExitStatus for T where T: std::future::Future<Output = std::io::Result<ExitStatus>> {}

#[allow(
    clippy::too_many_arguments,
    reason = "all fields belong to one completed process result"
)]
fn classify_process_result(
    file: &str,
    args: &[String],
    pid: u32,
    started: Instant,
    status: Option<ExitStatus>,
    forced_failure: Option<ForcedFailure>,
    stdout: String,
    stderr: String,
) -> Result<ProcessOutput, ProcessError> {
    let exit_code = status.and_then(|value| value.code());
    let signal = status.and_then(exit_signal);
    let elapsed = elapsed_ms(started);
    if let Some(failure) = forced_failure {
        let (message, timed_out, aborted) = forced_failure_fields(failure);
        return Err(ProcessError {
            message,
            exit_code,
            stdout: stdout.into_boxed_str(),
            stderr: stderr.into_boxed_str(),
            file: file.to_owned().into_boxed_str(),
            args: args.to_vec().into_boxed_slice(),
            timed_out,
            aborted,
            signal,
            elapsed_ms: elapsed,
        });
    }
    let Some(status) = status else {
        return Err(ProcessError {
            message: "Command process disappeared without an exit status.".to_owned(),
            exit_code: None,
            stdout: stdout.into_boxed_str(),
            stderr: stderr.into_boxed_str(),
            file: file.to_owned().into_boxed_str(),
            args: args.to_vec().into_boxed_slice(),
            timed_out: false,
            aborted: false,
            signal,
            elapsed_ms: elapsed,
        });
    };
    let code = status.code().unwrap_or(-1);
    if !status.success() {
        return Err(ProcessError {
            message: summarize_process_failure(file, code, &stdout, &stderr),
            exit_code: status.code(),
            stdout: stdout.into_boxed_str(),
            stderr: stderr.into_boxed_str(),
            file: file.to_owned().into_boxed_str(),
            args: args.to_vec().into_boxed_slice(),
            timed_out: false,
            aborted: false,
            signal,
            elapsed_ms: elapsed,
        });
    }
    Ok(ProcessOutput {
        stdout,
        stderr,
        exit_code: code,
        pid,
        elapsed_ms: elapsed,
    })
}

fn forced_failure_fields(failure: ForcedFailure) -> (String, bool, bool) {
    match failure {
        ForcedFailure::Cancelled => (
            "Command cancelled by the MCP client.".to_owned(),
            false,
            true,
        ),
        ForcedFailure::TimedOut(timeout) => (
            format!("Command timed out after {} ms.", timeout.as_millis()),
            true,
            false,
        ),
    }
}

fn launch_error(file: &str, args: &[String], message: String, started: Instant) -> ProcessError {
    ProcessError {
        message,
        exit_code: None,
        stdout: String::new().into_boxed_str(),
        stderr: String::new().into_boxed_str(),
        file: file.to_owned().into_boxed_str(),
        args: args.to_vec().into_boxed_slice(),
        timed_out: false,
        aborted: false,
        signal: None,
        elapsed_ms: elapsed_ms(started),
    }
}

#[must_use]
pub fn summarize_process_failure(file: &str, code: i32, stdout: &str, stderr: &str) -> String {
    let trimmed_stderr = stderr.trim();
    if !trimmed_stderr.is_empty() {
        if trimmed_stderr.chars().count() <= MAX_PROCESS_ERROR_MESSAGE_CHARS {
            return trimmed_stderr.to_owned();
        }
        let suffix = format!(
            "\n... error summary truncated to {MAX_PROCESS_ERROR_MESSAGE_CHARS} characters ..."
        );
        let keep = MAX_PROCESS_ERROR_MESSAGE_CHARS.saturating_sub(suffix.chars().count());
        return format!("{}{}", take_chars(trimmed_stderr, keep), suffix);
    }
    let stdout_chars = stdout.chars().count();
    if stdout_chars > 0 {
        format!(
            "{file} exited with code {code} after producing {stdout_chars} characters of stdout; see the bounded stdout field."
        )
    } else {
        format!("{file} exited with code {code}.")
    }
}

fn start_capture_task<R>(
    reader: R,
    stream: OutputStream,
    max_capture_chars: Option<usize>,
    output_tx: Option<Sender<OutputChunk>>,
) -> CaptureTask
where
    R: AsyncRead + Unpin + Send + 'static,
{
    let captured = Arc::new(Mutex::new(String::new()));
    let shared = captured.clone();
    let task = tokio::spawn(capture_stream(
        reader,
        stream,
        max_capture_chars,
        output_tx,
        shared,
    ));
    CaptureTask { task, captured }
}

async fn capture_stream<R>(
    mut reader: R,
    stream: OutputStream,
    max_capture_chars: Option<usize>,
    output_tx: Option<Sender<OutputChunk>>,
    captured: Arc<Mutex<String>>,
) where
    R: AsyncRead + Unpin,
{
    let mut local = String::new();
    let mut buffer = vec![0_u8; 16 * 1024];
    let mut pending_utf8 = Vec::with_capacity(4);
    loop {
        let count = match reader.read(&mut buffer).await {
            Ok(0) | Err(_) => break,
            Ok(count) => count,
        };
        let bytes = &buffer[..count];
        if let Some(tx) = output_tx.as_ref() {
            let _ = tx
                .send(OutputChunk {
                    stream,
                    bytes: Arc::from(bytes),
                })
                .await;
        }
        append_stream_utf8(&mut local, &mut pending_utf8, bytes);
        if let Some(limit) = max_capture_chars {
            retain_tail_chars(&mut local, limit);
        }
        captured
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone_from(&local);
    }
    if !pending_utf8.is_empty() {
        local.push_str(&String::from_utf8_lossy(&pending_utf8));
        if let Some(limit) = max_capture_chars {
            retain_tail_chars(&mut local, limit);
        }
    }
    *captured
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner) = local;
}

fn append_stream_utf8(captured: &mut String, pending: &mut Vec<u8>, bytes: &[u8]) {
    pending.extend_from_slice(bytes);
    loop {
        match std::str::from_utf8(pending) {
            Ok(text) => {
                captured.push_str(text);
                pending.clear();
                return;
            }
            Err(error) => {
                let valid_up_to = error.valid_up_to();
                if valid_up_to > 0 {
                    let valid = std::str::from_utf8(&pending[..valid_up_to])
                        .expect("valid_up_to must delimit valid UTF-8");
                    captured.push_str(valid);
                }
                if let Some(error_len) = error.error_len() {
                    captured.push('�');
                    pending.drain(..valid_up_to.saturating_add(error_len));
                } else {
                    pending.drain(..valid_up_to);
                    return;
                }
            }
        }
    }
}

async fn join_capture(mut capture: CaptureTask, timeout: Option<Duration>) -> String {
    if let Some(timeout) = timeout {
        if tokio::time::timeout(timeout, &mut capture.task)
            .await
            .is_err()
        {
            capture.task.abort();
            let _ = capture.task.await;
        }
    } else {
        capture.task.abort();
        let _ = capture.task.await;
    }
    capture
        .captured
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .clone()
}

fn retain_tail_chars(value: &mut String, max_chars: usize) {
    if max_chars == 0 {
        value.clear();
        return;
    }
    let count = value.chars().count();
    if count <= max_chars {
        return;
    }
    *value = value.chars().skip(count - max_chars).collect();
}

fn take_chars(value: &str, max_chars: usize) -> String {
    value.chars().take(max_chars).collect()
}

fn elapsed_ms(started: Instant) -> u64 {
    u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX)
}

#[cfg(windows)]
fn configure_process_group(command: &mut Command) {
    use std::os::windows::process::CommandExt as _;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    command.as_std_mut().creation_flags(CREATE_NO_WINDOW);
}

#[cfg(unix)]
fn configure_process_group(command: &mut Command) {
    use std::os::unix::process::CommandExt as _;
    command.as_std_mut().process_group(0);
}

#[cfg(not(any(windows, unix)))]
fn configure_process_group(_command: &mut Command) {}

#[cfg(windows)]
fn terminate_process_tree_on_drop(pid: u32, job: Option<&crate::windows_job::WindowsJob>) {
    terminate_spawned_tree(pid, job);
}

#[cfg(windows)]
fn terminate_spawned_tree(pid: u32, job: Option<&crate::windows_job::WindowsJob>) {
    if pid == 0 {
        return;
    }
    if let Some(job) = job {
        let _ = job.terminate(1);
    }
    // Always sweep the native process tree as well. A child could theoretically
    // create a descendant in the tiny interval between CreateProcess and Job Object assignment.
    crate::windows_job::terminate_process_tree_fallback(pid, 1);
}

#[cfg(unix)]
fn terminate_process_tree_on_drop(pid: u32) {
    use nix::{
        sys::signal::{Signal, killpg},
        unistd::Pid,
    };
    if let Ok(raw_pid) = i32::try_from(pid) {
        let _ = killpg(Pid::from_raw(raw_pid), Signal::SIGKILL);
    }
}

#[cfg(not(any(windows, unix)))]
fn terminate_process_tree_on_drop(_pid: u32) {}

#[cfg(windows)]
pub(crate) async fn terminate_process_tree(pid: u32) {
    crate::windows_job::terminate_process_tree_fallback(pid, 1);
}

#[cfg(unix)]
pub(crate) async fn terminate_process_tree(pid: u32) {
    use nix::{
        sys::signal::{Signal, killpg},
        unistd::Pid,
    };
    let Ok(raw_pid) = i32::try_from(pid) else {
        return;
    };
    let group = Pid::from_raw(raw_pid);
    let _ = killpg(group, Signal::SIGTERM);
    tokio::time::sleep(Duration::from_secs(1)).await;
    let _ = killpg(group, Signal::SIGKILL);
}

#[cfg(not(any(windows, unix)))]
async fn terminate_process_tree(_pid: u32) {}

#[cfg(unix)]
fn exit_signal(status: ExitStatus) -> Option<i32> {
    use std::os::unix::process::ExitStatusExt as _;
    status.signal()
}

#[cfg(not(unix))]
fn exit_signal(_status: ExitStatus) -> Option<i32> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sleeping_command() -> (&'static str, Vec<String>) {
        #[cfg(windows)]
        {
            (
                "powershell.exe",
                vec![
                    "-NoLogo".to_owned(),
                    "-NoProfile".to_owned(),
                    "-NonInteractive".to_owned(),
                    "-Command".to_owned(),
                    "Start-Sleep -Seconds 30".to_owned(),
                ],
            )
        }
        #[cfg(unix)]
        {
            ("/bin/sh", vec!["-lc".to_owned(), "sleep 30".to_owned()])
        }
        #[cfg(not(any(windows, unix)))]
        {
            ("", Vec::new())
        }
    }

    fn output_command() -> (&'static str, Vec<String>) {
        #[cfg(windows)]
        {
            (
                "powershell.exe",
                vec![
                    "-NoLogo".to_owned(),
                    "-NoProfile".to_owned(),
                    "-NonInteractive".to_owned(),
                    "-Command".to_owned(),
                    "[Console]::Out.Write('abcdefgh'); [Console]::Error.Write('12345678')"
                        .to_owned(),
                ],
            )
        }
        #[cfg(unix)]
        {
            (
                "/bin/sh",
                vec![
                    "-lc".to_owned(),
                    "printf abcdefgh; printf 12345678 >&2".to_owned(),
                ],
            )
        }
        #[cfg(not(any(windows, unix)))]
        {
            ("", Vec::new())
        }
    }

    #[tokio::test]
    async fn cancellation_terminates_a_running_process_promptly() {
        let (file, args) = sleeping_command();
        let cancellation = CancellationToken::new();
        let cancel = cancellation.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(100)).await;
            cancel.cancel();
        });
        let started = Instant::now();
        let error = spawn_process(file, &args, ProcessOptions::default(), cancellation)
            .await
            .expect_err("process should be cancelled");
        assert!(error.aborted);
        assert!(!error.timed_out);
        assert!(started.elapsed() < Duration::from_secs(5));
    }

    #[tokio::test]
    async fn dropping_spawn_future_terminates_the_child_process() {
        let (file, args) = sleeping_command();
        let (pid_tx, mut pid_rx) = tokio::sync::mpsc::unbounded_channel();
        let task = tokio::spawn(async move {
            spawn_process(
                file,
                &args,
                ProcessOptions {
                    timeout: Some(Duration::from_secs(30)),
                    pid_tx: Some(pid_tx),
                    ..ProcessOptions::default()
                },
                CancellationToken::new(),
            )
            .await
        });
        let pid = tokio::time::timeout(Duration::from_secs(5), pid_rx.recv())
            .await
            .expect("pid timeout")
            .expect("pid");
        task.abort();
        let _ = task.await;
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && process_is_alive(pid).await {
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        assert!(
            !process_is_alive(pid).await,
            "dropped process future left PID {pid} alive"
        );
    }

    #[tokio::test]
    async fn parent_exit_is_not_blocked_by_grandchild_inheriting_pipes() {
        let script = "const {spawn}=require('node:child_process'); const c=spawn(process.execPath,['-e','setTimeout(()=>{},3000)'],{stdio:['ignore','inherit','inherit']}); c.unref(); process.stdout.write('parent-done');";
        let started = Instant::now();
        let output = spawn_process(
            "node",
            &["-e".to_owned(), script.to_owned()],
            ProcessOptions {
                timeout: Some(Duration::from_secs(5)),
                ..ProcessOptions::default()
            },
            CancellationToken::new(),
        )
        .await
        .expect("parent process should complete without waiting for inherited grandchild pipes");
        assert_eq!(output.exit_code, 0);
        assert!(output.stdout.contains("parent-done"));
        assert!(started.elapsed() < Duration::from_millis(2500));
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn windows_timeout_terminates_job_object_descendants() {
        let temp = tempfile::tempdir().unwrap();
        let pid_path = temp.path().join("descendant.pid");
        let pid_path_js = pid_path
            .to_string_lossy()
            .replace('\\', "\\\\")
            .replace('\'', "\\'");
        let script = format!(
            "const fs=require('node:fs'); const {{spawn}}=require('node:child_process'); const c=spawn(process.execPath,['-e','setTimeout(()=>{{}},30000)'],{{stdio:['ignore','ignore','ignore']}}); fs.writeFileSync('{pid_path_js}',String(c.pid)); setTimeout(()=>{{}},30000);"
        );
        let error = spawn_process(
            "node",
            &["-e".to_owned(), script],
            ProcessOptions {
                timeout: Some(Duration::from_millis(1200)),
                ..ProcessOptions::default()
            },
            CancellationToken::new(),
        )
        .await
        .expect_err("parent should time out");
        assert!(error.timed_out);
        let child_pid = std::fs::read_to_string(&pid_path)
            .expect("parent should persist descendant pid before timeout")
            .trim()
            .parse::<u32>()
            .expect("valid descendant pid");
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline && crate::windows_process::process_alive(child_pid) {
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        assert!(
            !crate::windows_process::process_alive(child_pid),
            "Job Object timeout left descendant PID {child_pid} alive"
        );
    }

    #[tokio::test]
    async fn timeout_terminates_a_running_process_promptly() {
        let (file, args) = sleeping_command();
        let error = spawn_process(
            file,
            &args,
            ProcessOptions {
                timeout: Some(Duration::from_millis(100)),
                ..ProcessOptions::default()
            },
            CancellationToken::new(),
        )
        .await
        .expect_err("process should time out");
        assert!(error.timed_out);
        assert!(!error.aborted);
        assert!(error.message.contains("100 ms"));
    }

    #[test]
    fn streaming_utf8_decoder_preserves_split_multibyte_sequences() {
        let text = "A🙂中B";
        let bytes = text.as_bytes();
        let mut captured = String::new();
        let mut pending = Vec::new();
        append_stream_utf8(&mut captured, &mut pending, &bytes[..3]);
        append_stream_utf8(&mut captured, &mut pending, &bytes[3..6]);
        append_stream_utf8(&mut captured, &mut pending, &bytes[6..]);
        assert!(pending.is_empty());
        assert_eq!(captured, text);
    }

    #[test]
    fn streaming_utf8_decoder_replaces_only_invalid_sequences() {
        let mut captured = String::new();
        let mut pending = Vec::new();
        append_stream_utf8(&mut captured, &mut pending, b"a\xffb");
        assert!(pending.is_empty());
        assert_eq!(captured, "a�b");
    }

    #[tokio::test]
    async fn output_capture_keeps_bounded_tails() {
        let (file, args) = output_command();
        let output = spawn_process(
            file,
            &args,
            ProcessOptions {
                max_capture_chars: Some(4),
                ..ProcessOptions::default()
            },
            CancellationToken::new(),
        )
        .await
        .expect("output process succeeds");
        assert_eq!(output.stdout, "efgh");
        assert_eq!(output.stderr, "5678");
    }

    #[test]
    fn stderr_failure_summary_is_bounded() {
        let stderr = "failure".repeat(MAX_PROCESS_ERROR_MESSAGE_CHARS);
        let summary = summarize_process_failure("tool", 9, "", &stderr);
        assert!(summary.chars().count() <= MAX_PROCESS_ERROR_MESSAGE_CHARS);
        assert!(summary.contains("error summary truncated"));
    }

    #[cfg(windows)]
    async fn process_is_alive(pid: u32) -> bool {
        let filter = format!("PID eq {pid}");
        let Ok(output) = Command::new("tasklist.exe")
            .args(["/fi", &filter, "/fo", "csv", "/nh"])
            .output()
            .await
        else {
            return false;
        };
        let stdout = String::from_utf8_lossy(&output.stdout);
        stdout
            .lines()
            .filter_map(|line| line.split(',').nth(1))
            .map(|value| value.trim().trim_matches('"'))
            .any(|value| value == pid.to_string())
    }

    #[cfg(unix)]
    fn process_is_alive(pid: u32) -> std::future::Ready<bool> {
        use nix::{sys::signal::kill, unistd::Pid};
        let alive = i32::try_from(pid)
            .ok()
            .is_some_and(|value| kill(Pid::from_raw(value), None).is_ok());
        std::future::ready(alive)
    }

    #[cfg(not(any(windows, unix)))]
    fn process_is_alive(_pid: u32) -> std::future::Ready<bool> {
        std::future::ready(false)
    }
}
