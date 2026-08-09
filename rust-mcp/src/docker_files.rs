use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
    time::Duration,
};

use tokio::sync::RwLock;
use tokio_util::sync::CancellationToken;

use crate::{
    Config,
    files::{LargeReadResult, LargeWriteResult, ProcessResult},
    process::{ProcessError, ProcessOptions, spawn_process},
};

const LIST_PYTHON: &str = r#"
import json
import os
import stat
import sys
import time

root = sys.argv[1]
recursive = sys.argv[2] == "1"
max_depth = max(1, int(sys.argv[3]))
max_entries = max(1, int(sys.argv[4]))
timeout_ms = max(1, int(sys.argv[5]))
excluded = {str(x).lower() for x in json.loads(sys.argv[6])}
deadline = time.monotonic() + timeout_ms / 1000.0
entries = []
pruned = 0
skipped = 0
timed_out = False
truncated = False
stack = [(root, 0)]

while stack:
    if time.monotonic() >= deadline:
        timed_out = True
        break
    current, depth = stack.pop()
    try:
        info = os.lstat(current)
    except (FileNotFoundError, PermissionError, NotADirectoryError):
        skipped += 1
        continue
    mode = info.st_mode
    kind = "l" if stat.S_ISLNK(mode) else "d" if stat.S_ISDIR(mode) else "f" if stat.S_ISREG(mode) else "?"
    entries.append(f"{kind}\t{current}")
    if len(entries) >= max_entries:
        truncated = True
        break
    if not recursive or not stat.S_ISDIR(mode) or depth >= max_depth:
        continue
    try:
        children = sorted(os.scandir(current), key=lambda e: (e.name.lower(), e.name), reverse=True)
    except (FileNotFoundError, PermissionError, NotADirectoryError):
        skipped += 1
        continue
    for child in children:
        if child.name.lower() in excluded:
            pruned += 1
            continue
        stack.append((child.path, depth + 1))

if entries:
    sys.stdout.write("\n".join(entries) + "\n")
notices = []
if timed_out:
    notices.append(f"listing stopped after {timeout_ms} ms")
if truncated:
    notices.append(f"listing capped at {max_entries} entries")
if pruned:
    notices.append(f"pruned {pruned} excluded directories")
if skipped:
    notices.append(f"skipped {skipped} inaccessible or vanished paths")
if notices:
    sys.stderr.write("; ".join(notices) + "\n")
"#;

const READ_TEXT_PYTHON: &str = r#"
import os
import stat
import sys

path = sys.argv[1]
max_bytes = max(1, int(sys.argv[2]))
info = os.stat(path)
if not stat.S_ISREG(info.st_mode):
    raise SystemExit("Not a regular file.")
with open(path, "rb") as handle:
    sys.stdout.buffer.write(handle.read(max_bytes))
"#;

const WRITE_TEXT_PYTHON: &str = r#"
import os
import sys

path = sys.argv[1]
append = sys.argv[2] == "1"
create_dirs = sys.argv[3] == "1"
parent = os.path.dirname(path)
if create_dirs and parent and parent != ".":
    os.makedirs(parent, exist_ok=True)
with open(path, "ab" if append else "wb") as handle:
    handle.write(sys.stdin.buffer.read())
"#;

const LARGE_READ_PYTHON: &str = r#"
import base64
import hashlib
import json
import os
import stat as statmod
import sys

file_path = sys.argv[1]
offset_requested = max(0, int(sys.argv[2]))
bytes_requested = max(1, int(sys.argv[3]))
file_stat = os.stat(file_path)
if not statmod.S_ISREG(file_stat.st_mode):
    raise SystemExit("Not a regular file.")
actual_offset = min(offset_requested, file_stat.st_size)
bytes_to_read = max(0, min(bytes_requested, file_stat.st_size - actual_offset))
with open(file_path, "rb") as handle:
    handle.seek(actual_offset)
    chunk = handle.read(bytes_to_read)
result = {
    "path": file_path,
    "file_size": file_stat.st_size,
    "offset_bytes_requested": offset_requested,
    "offset_bytes": actual_offset,
    "bytes_requested": bytes_requested,
    "bytes_returned": len(chunk),
    "next_offset_bytes": actual_offset + len(chunk),
    "eof": actual_offset + len(chunk) >= file_stat.st_size,
    "content_sha256": hashlib.sha256(chunk).hexdigest(),
    "content_base64": base64.b64encode(chunk).decode("ascii"),
}
sys.stdout.write(json.dumps(result))
"#;

const LARGE_WRITE_PYTHON: &str = r#"
import base64
import binascii
import hashlib
import json
import os
import re
import stat as statmod
import sys

def fail(message):
    raise SystemExit(message)

file_path = sys.argv[1]
append = sys.argv[2] == "1"
create_dirs = sys.argv[3] == "1"
expected_sha256 = sys.argv[4].strip().lower() if len(sys.argv) > 4 and sys.argv[4] else ""
stdin_base64 = sys.stdin.read()
normalized = "".join(stdin_base64.split())
if normalized:
    try:
        payload = base64.b64decode(normalized, validate=True)
    except binascii.Error:
        fail("Invalid base64 payload.")
    if base64.b64encode(payload).decode("ascii") != normalized:
        fail("Invalid base64 payload.")
else:
    payload = b""
content_sha256 = hashlib.sha256(payload).hexdigest()
if expected_sha256:
    if not re.fullmatch(r"[a-f0-9]{64}", expected_sha256):
        fail("expected_sha256 must be a 64-character SHA-256 hex string.")
    if expected_sha256 != content_sha256:
        fail("Decoded payload SHA-256 did not match expected_sha256.")
target_existed = False
previous_file_size = 0
try:
    initial_stat = os.stat(file_path)
    if not statmod.S_ISREG(initial_stat.st_mode):
        fail("Target exists but is not a regular file.")
    target_existed = True
    previous_file_size = initial_stat.st_size
except FileNotFoundError:
    pass
parent_dir = os.path.dirname(file_path)
if create_dirs and parent_dir and parent_dir != ".":
    os.makedirs(parent_dir, exist_ok=True)
with open(file_path, "ab" if append else "wb") as handle:
    handle.write(payload)
final_stat = os.stat(file_path)
if not statmod.S_ISREG(final_stat.st_mode):
    fail("Target is not a regular file after write.")
verified = False
verification_mode = ""
file_sha256 = None
if append:
    verification_mode = "suffix-bytes"
    if len(payload) == 0:
        verified = True
    else:
        with open(file_path, "rb") as handle:
            handle.seek(previous_file_size)
            verified = handle.read(len(payload)) == payload
else:
    verification_mode = "whole-file-sha256"
    digest = hashlib.sha256()
    with open(file_path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    file_sha256 = digest.hexdigest()
    verified = final_stat.st_size == len(payload) and file_sha256 == content_sha256
if not verified:
    fail("Mirror verification failed after writing the payload.")
result = {
    "path": file_path,
    "append": bool(append),
    "previous_file_size": previous_file_size,
    "final_file_size": final_stat.st_size,
    "bytes_written": len(payload),
    "content_sha256": content_sha256,
    "verification_mode": verification_mode,
    "verified": verified,
    "expected_sha256_verified": True if expected_sha256 else None,
    "target_existed": target_existed,
    "file_sha256": file_sha256,
}
sys.stdout.write(json.dumps(result))
"#;

#[derive(Debug, Clone)]
pub struct DockerFileError {
    pub message: String,
    pub exit_code: Option<i32>,
    pub stdout: String,
    pub stderr: String,
}

impl std::fmt::Display for DockerFileError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for DockerFileError {}

impl From<ProcessError> for DockerFileError {
    fn from(error: ProcessError) -> Self {
        Self {
            message: error.message,
            exit_code: error.exit_code,
            stdout: error.stdout.into_string(),
            stderr: error.stderr.into_string(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct DockerListOptions {
    pub path: String,
    pub recursive: bool,
    pub max_depth: usize,
    pub max_entries: usize,
    pub timeout: Duration,
    pub exclude_directories: Vec<String>,
}

#[derive(Debug, Default)]
pub struct DockerFileBackend {
    locks: Mutex<HashMap<String, Arc<RwLock<()>>>>,
}

impl DockerFileBackend {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// List bounded file metadata inside the configured Devbox container.
    ///
    /// # Errors
    /// Returns an error when Docker execution fails or the exclusion list cannot be serialized.
    pub async fn list(
        &self,
        config: &Config,
        options: &DockerListOptions,
        cancellation: CancellationToken,
    ) -> Result<ProcessResult, DockerFileError> {
        let excluded = serde_json::to_string(&options.exclude_directories).map_err(|error| {
            DockerFileError {
                message: format!("serialize excluded directories: {error}"),
                exit_code: None,
                stdout: String::new(),
                stderr: String::new(),
            }
        })?;
        let args = vec![
            options.path.clone(),
            if options.recursive { "1" } else { "0" }.to_owned(),
            options.max_depth.max(1).to_string(),
            options.max_entries.max(1).to_string(),
            options.timeout.as_millis().max(1).to_string(),
            excluded,
        ];
        let output = self
            .run_python(
                config,
                LIST_PYTHON,
                args,
                None,
                options.timeout,
                cancellation,
            )
            .await?;
        Ok(ProcessResult {
            stdout: output.stdout,
            stderr: output.stderr,
            exit_code: output.exit_code,
        })
    }

    /// Read a bounded text prefix from a regular file inside the Devbox container.
    ///
    /// # Errors
    /// Returns an error when Docker execution fails or the path is not a regular readable file.
    pub async fn read_text(
        &self,
        config: &Config,
        path: &str,
        max_bytes: usize,
        cancellation: CancellationToken,
    ) -> Result<ProcessResult, DockerFileError> {
        let lock = self.lock_for(config, path);
        let _guard = lock.read().await;
        let output = self
            .run_python(
                config,
                READ_TEXT_PYTHON,
                vec![path.to_owned(), max_bytes.max(1).to_string()],
                None,
                Duration::from_secs(30),
                cancellation,
            )
            .await?;
        Ok(ProcessResult {
            stdout: output.stdout,
            stderr: output.stderr,
            exit_code: output.exit_code,
        })
    }

    /// Write or append UTF-8 text inside the Devbox container.
    ///
    /// # Errors
    /// Returns an error when Docker execution or the container-side write fails.
    pub async fn write_text(
        &self,
        config: &Config,
        path: &str,
        content: &str,
        append: bool,
        create_dirs: bool,
        cancellation: CancellationToken,
    ) -> Result<ProcessResult, DockerFileError> {
        let lock = self.lock_for(config, path);
        let _guard = lock.write().await;
        let output = self
            .run_python(
                config,
                WRITE_TEXT_PYTHON,
                vec![
                    path.to_owned(),
                    if append { "1" } else { "0" }.to_owned(),
                    if create_dirs { "1" } else { "0" }.to_owned(),
                ],
                Some(content.as_bytes().to_vec()),
                Duration::from_secs(30),
                cancellation,
            )
            .await?;
        Ok(ProcessResult {
            stdout: output.stdout,
            stderr: output.stderr,
            exit_code: output.exit_code,
        })
    }

    /// Read an exact byte range from a regular file inside the Devbox container.
    ///
    /// # Errors
    /// Returns an error when Docker execution fails or the helper returns invalid structured data.
    pub async fn read_large(
        &self,
        config: &Config,
        path: &str,
        offset_bytes: u64,
        max_bytes: usize,
        cancellation: CancellationToken,
    ) -> Result<LargeReadResult, DockerFileError> {
        let lock = self.lock_for(config, path);
        let _guard = lock.read().await;
        let output = self
            .run_python(
                config,
                LARGE_READ_PYTHON,
                vec![
                    path.to_owned(),
                    offset_bytes.to_string(),
                    max_bytes.max(1).to_string(),
                ],
                None,
                Duration::from_secs(120),
                cancellation,
            )
            .await?;
        serde_json::from_str(&output.stdout).map_err(|error| DockerFileError {
            message: format!("Large file read for {path} returned invalid JSON: {error}"),
            exit_code: Some(output.exit_code),
            stdout: output.stdout,
            stderr: output.stderr,
        })
    }

    /// Write exact bytes inside the Devbox container and verify the persisted payload.
    ///
    /// # Errors
    /// Returns an error for invalid payloads, Docker execution failures, or verification failures.
    #[allow(
        clippy::too_many_arguments,
        reason = "mirrors the MCP exact-write contract"
    )]
    pub async fn write_large(
        &self,
        config: &Config,
        path: &str,
        content_base64: &str,
        append: bool,
        create_dirs: bool,
        expected_sha256: Option<&str>,
        cancellation: CancellationToken,
    ) -> Result<LargeWriteResult, DockerFileError> {
        let lock = self.lock_for(config, path);
        let _guard = lock.write().await;
        let output = self
            .run_python(
                config,
                LARGE_WRITE_PYTHON,
                vec![
                    path.to_owned(),
                    if append { "1" } else { "0" }.to_owned(),
                    if create_dirs { "1" } else { "0" }.to_owned(),
                    expected_sha256.unwrap_or_default().to_owned(),
                ],
                Some(content_base64.as_bytes().to_vec()),
                Duration::from_secs(120),
                cancellation,
            )
            .await?;
        serde_json::from_str(&output.stdout).map_err(|error| DockerFileError {
            message: format!("Large file write for {path} returned invalid JSON: {error}"),
            exit_code: Some(output.exit_code),
            stdout: output.stdout,
            stderr: output.stderr,
        })
    }

    async fn run_python(
        &self,
        config: &Config,
        script: &str,
        script_args: Vec<String>,
        input: Option<Vec<u8>>,
        timeout: Duration,
        cancellation: CancellationToken,
    ) -> Result<crate::process::ProcessOutput, DockerFileError> {
        let mut args = vec!["exec".to_owned()];
        if input.is_some() {
            args.push("-i".to_owned());
        }
        if !config.devbox_default_user.is_empty() {
            args.extend(["-u".to_owned(), config.devbox_default_user.clone()]);
        }
        args.extend([
            "-w".to_owned(),
            config.devbox_workspace_path.to_string_lossy().into_owned(),
            config.devbox_container_name.clone(),
            "python3".to_owned(),
            "-c".to_owned(),
            script.to_owned(),
        ]);
        args.extend(script_args);
        spawn_process(
            "docker",
            &args,
            ProcessOptions {
                timeout: Some(timeout),
                input,
                max_capture_chars: Some(config.max_mcp_transfer_chars.min(8_000_000)),
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await
        .map_err(Into::into)
    }

    fn lock_for(&self, config: &Config, path: &str) -> Arc<RwLock<()>> {
        let key = normalize_posix_key(&config.devbox_workspace_path.to_string_lossy(), path);
        let mut locks = self
            .locks
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        locks
            .entry(key)
            .or_insert_with(|| Arc::new(RwLock::new(())))
            .clone()
    }
}

fn normalize_posix_key(workspace: &str, path: &str) -> String {
    let combined = if path.starts_with('/') {
        path.to_owned()
    } else {
        format!("{}/{}", workspace.trim_end_matches('/'), path)
    };
    let mut pieces = Vec::new();
    for piece in combined.split('/') {
        match piece {
            "" | "." => {}
            ".." => {
                pieces.pop();
            }
            other => pieces.push(other),
        }
    }
    format!("/{}", pieces.join("/"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn docker_lock_key_matches_posix_normalization() {
        assert_eq!(
            normalize_posix_key("/workspace", "a/../b.txt"),
            "/workspace/b.txt"
        );
        assert_eq!(normalize_posix_key("/workspace", "/tmp/./x"), "/tmp/x");
    }

    #[test]
    fn listing_script_does_not_interpolate_user_paths() {
        assert!(!LIST_PYTHON.contains("{path}"));
        assert!(LIST_PYTHON.contains("sys.argv[1]"));
    }

    #[test]
    fn docker_error_can_be_serialized_for_tool_envelope_data() {
        let error = DockerFileError {
            message: "failure".to_owned(),
            exit_code: Some(1),
            stdout: "out".to_owned(),
            stderr: "err".to_owned(),
        };
        let value = json!({
            "exitCode": error.exit_code,
            "stdout": error.stdout,
            "stderr": error.stderr,
        });
        assert_eq!(value["exitCode"], 1);
    }
}
