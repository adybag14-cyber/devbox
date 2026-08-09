use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::Serialize;
use tokio::{
    fs::{self, OpenOptions},
    io::AsyncWriteExt,
    sync::mpsc::{self, UnboundedSender},
    task::JoinHandle,
};

use crate::process::{OutputChunk, OutputStream};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RotatingLogSnapshot {
    pub max_bytes: u64,
    pub rotations: usize,
    pub rotations_performed: u64,
    pub total_bytes: u64,
    pub current_bytes: u64,
    pub truncated: bool,
    pub failed: bool,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct JobLogSnapshots {
    pub stdout: RotatingLogSnapshot,
    pub stderr: RotatingLogSnapshot,
    pub truncated: bool,
}

pub struct JobLogPump {
    tx: Option<UnboundedSender<OutputChunk>>,
    task: JoinHandle<Result<JobLogSnapshots>>,
}

impl JobLogPump {
    /// Start a single ordered log-writer task for process stdout/stderr chunks.
    ///
    /// # Errors
    /// Returns an error when either current log file cannot be opened.
    pub async fn start(
        stdout_path: PathBuf,
        stderr_path: PathBuf,
        max_bytes: u64,
        rotations: usize,
    ) -> Result<Self> {
        let stdout = RotatingSink::open(stdout_path, max_bytes, rotations).await?;
        let stderr = RotatingSink::open(stderr_path, max_bytes, rotations).await?;
        let (tx, mut rx) = mpsc::unbounded_channel::<OutputChunk>();
        let task = tokio::spawn(async move {
            let mut stdout = stdout;
            let mut stderr = stderr;
            while let Some(chunk) = rx.recv().await {
                let target = match chunk.stream {
                    OutputStream::Stdout => &mut stdout,
                    OutputStream::Stderr => &mut stderr,
                };
                if let Err(error) = target.write(&chunk.bytes).await {
                    target.record_failure(&error);
                }
            }
            let stdout_snapshot = stdout.finish().await;
            let stderr_snapshot = stderr.finish().await;
            let truncated = stdout_snapshot.truncated || stderr_snapshot.truncated;
            Ok(JobLogSnapshots {
                stdout: stdout_snapshot,
                stderr: stderr_snapshot,
                truncated,
            })
        });
        Ok(Self { tx: Some(tx), task })
    }

    #[must_use]
    pub fn sender(&self) -> Option<UnboundedSender<OutputChunk>> {
        self.tx.clone()
    }

    /// Close the channel, flush both streams, and return the final log metadata.
    ///
    /// # Errors
    /// Returns an error if the writer task panics or cannot finish.
    pub async fn finish(mut self) -> Result<JobLogSnapshots> {
        self.tx.take();
        self.task.await.context("join job log writer")?
    }
}

struct RotatingSink {
    path: PathBuf,
    file: Option<fs::File>,
    max_bytes: u64,
    rotations: usize,
    current_bytes: u64,
    total_bytes: u64,
    rotations_performed: u64,
    failure: Option<String>,
}

impl RotatingSink {
    async fn open(path: PathBuf, max_bytes: u64, rotations: usize) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).await?;
        }
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .await
            .with_context(|| format!("open rotating log {}", path.display()))?;
        let current_bytes = file.metadata().await?.len();
        Ok(Self {
            path,
            file: Some(file),
            max_bytes: max_bytes.max(4096),
            rotations,
            current_bytes,
            total_bytes: 0,
            rotations_performed: 0,
            failure: None,
        })
    }

    async fn write(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.is_empty() || self.failure.is_some() {
            return Ok(());
        }
        let mut remaining = bytes;
        while !remaining.is_empty() {
            if self.current_bytes >= self.max_bytes {
                self.rotate().await?;
            }
            let available = self.max_bytes.saturating_sub(self.current_bytes).max(1);
            let slice_len = remaining
                .len()
                .min(usize::try_from(available).unwrap_or(usize::MAX));
            let (slice, rest) = remaining.split_at(slice_len);
            let file = self
                .file
                .as_mut()
                .context("rotating log has no writable file")?;
            file.write_all(slice).await?;
            let written = u64::try_from(slice.len()).unwrap_or(u64::MAX);
            self.current_bytes = self.current_bytes.saturating_add(written);
            self.total_bytes = self.total_bytes.saturating_add(written);
            remaining = rest;
        }
        Ok(())
    }

    async fn rotate(&mut self) -> Result<()> {
        if let Some(mut file) = self.file.take() {
            file.flush().await?;
            drop(file);
        }
        rotate_files(&self.path, self.rotations).await?;
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&self.path)
            .await?;
        self.current_bytes = file.metadata().await?.len();
        self.file = Some(file);
        self.rotations_performed = self.rotations_performed.saturating_add(1);
        Ok(())
    }

    fn record_failure(&mut self, error: &anyhow::Error) {
        if self.failure.is_none() {
            self.failure = Some(error.to_string());
        }
    }

    async fn finish(mut self) -> RotatingLogSnapshot {
        if let Some(mut file) = self.file.take()
            && let Err(error) = file.flush().await
        {
            self.record_failure(&error.into());
        }
        if let Ok(metadata) = fs::metadata(&self.path).await {
            self.current_bytes = metadata.len();
        }
        RotatingLogSnapshot {
            max_bytes: self.max_bytes,
            rotations: self.rotations,
            rotations_performed: self.rotations_performed,
            total_bytes: self.total_bytes,
            current_bytes: self.current_bytes,
            truncated: self.rotations_performed > 0,
            failed: self.failure.is_some(),
            error: self.failure,
        }
    }
}

async fn rotate_files(path: &Path, rotations: usize) -> Result<()> {
    if rotations == 0 {
        remove_if_exists(path).await?;
        return Ok(());
    }
    remove_if_exists(&rotated_path(path, rotations)).await?;
    for index in (1..rotations).rev() {
        let source = rotated_path(path, index);
        let target = rotated_path(path, index + 1);
        rename_if_exists(&source, &target).await?;
    }
    rename_if_exists(path, &rotated_path(path, 1)).await
}

async fn rename_if_exists(source: &Path, target: &Path) -> Result<()> {
    match fs::rename(source, target).await {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
    }
}

async fn remove_if_exists(path: &Path) -> Result<()> {
    match fs::remove_file(path).await {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
    }
}

fn rotated_path(path: &Path, index: usize) -> PathBuf {
    PathBuf::from(format!("{}.{index}", path.to_string_lossy()))
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;

    #[tokio::test]
    async fn log_pump_rotates_without_exceeding_segment_limit() {
        let temp = tempfile::tempdir().unwrap();
        let stdout = temp.path().join("stdout.log");
        let stderr = temp.path().join("stderr.log");
        let pump = JobLogPump::start(stdout.clone(), stderr.clone(), 4096, 2)
            .await
            .unwrap();
        let sender = pump.sender().unwrap();
        sender
            .send(OutputChunk {
                stream: OutputStream::Stdout,
                bytes: Arc::from(vec![b'a'; 9000]),
            })
            .unwrap();
        sender
            .send(OutputChunk {
                stream: OutputStream::Stderr,
                bytes: Arc::from(b"warning".as_slice()),
            })
            .unwrap();
        drop(sender);
        let snapshot = pump.finish().await.unwrap();
        assert_eq!(snapshot.stdout.total_bytes, 9000);
        assert_eq!(snapshot.stdout.rotations_performed, 2);
        assert!(snapshot.stdout.truncated);
        assert!(!snapshot.stdout.failed);
        assert_eq!(fs::metadata(&stdout).await.unwrap().len(), 808);
        assert_eq!(
            fs::metadata(rotated_path(&stdout, 1)).await.unwrap().len(),
            4096
        );
        assert_eq!(
            fs::metadata(rotated_path(&stdout, 2)).await.unwrap().len(),
            4096
        );
        assert_eq!(fs::read_to_string(&stderr).await.unwrap(), "warning");
    }

    #[tokio::test]
    async fn zero_rotations_keeps_only_latest_segment() {
        let temp = tempfile::tempdir().unwrap();
        let stdout = temp.path().join("stdout.log");
        let stderr = temp.path().join("stderr.log");
        let pump = JobLogPump::start(stdout.clone(), stderr, 4096, 0)
            .await
            .unwrap();
        let sender = pump.sender().unwrap();
        sender
            .send(OutputChunk {
                stream: OutputStream::Stdout,
                bytes: Arc::from(vec![b'b'; 5000]),
            })
            .unwrap();
        drop(sender);
        let snapshot = pump.finish().await.unwrap();
        assert_eq!(snapshot.stdout.rotations_performed, 1);
        assert_eq!(fs::metadata(&stdout).await.unwrap().len(), 904);
        assert!(!rotated_path(&stdout, 1).exists());
    }
}
