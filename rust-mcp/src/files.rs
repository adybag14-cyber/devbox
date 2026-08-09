use std::{
    collections::{HashMap, HashSet},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use serde::Serialize;
use sha2::{Digest, Sha256};
use tokio::{
    fs::{self, File, OpenOptions},
    io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt, SeekFrom},
    sync::RwLock,
};
use tokio_util::sync::CancellationToken;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct ProcessResult {
    pub stdout: String,
    pub stderr: String,
    #[serde(rename = "exitCode")]
    pub exit_code: i32,
}

impl ProcessResult {
    #[must_use]
    pub fn success(stdout: String, stderr: String) -> Self {
        Self {
            stdout,
            stderr,
            exit_code: 0,
        }
    }
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LargeReadResult {
    pub path: String,
    pub file_size: u64,
    pub offset_bytes_requested: u64,
    pub offset_bytes: u64,
    pub bytes_requested: u64,
    pub bytes_returned: u64,
    pub next_offset_bytes: u64,
    pub eof: bool,
    pub content_sha256: String,
    pub content_base64: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct LargeWriteResult {
    pub path: String,
    pub append: bool,
    pub previous_file_size: u64,
    pub final_file_size: u64,
    pub bytes_written: u64,
    pub content_sha256: String,
    pub verification_mode: String,
    pub verified: bool,
    pub expected_sha256_verified: Option<bool>,
    pub target_existed: bool,
    pub file_sha256: Option<String>,
}

#[derive(Debug, Clone)]
pub struct ListOptions {
    pub path: PathBuf,
    pub recursive: bool,
    pub max_depth: usize,
    pub max_entries: usize,
    pub timeout: Duration,
    pub exclude_directories: Vec<String>,
}

#[derive(Debug, Default)]
pub struct FileService {
    locks: Mutex<HashMap<PathBuf, Arc<RwLock<()>>>>,
}

impl FileService {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Read at most `max_bytes` and decode with UTF-8 replacement semantics.
    ///
    /// # Errors
    /// Returns an error when the path cannot be read as a regular file.
    pub async fn read_text(&self, path: &Path, max_bytes: usize) -> Result<ProcessResult> {
        let lock = self.lock_for(path);
        let _guard = lock.read().await;
        let metadata = fs::metadata(path)
            .await
            .with_context(|| format!("read metadata for {}", path.display()))?;
        if !metadata.is_file() {
            bail!("Not a regular file.");
        }
        let bytes = fs::read(path)
            .await
            .with_context(|| format!("read {}", path.display()))?;
        let end = bytes.len().min(max_bytes.max(1));
        Ok(ProcessResult::success(
            String::from_utf8_lossy(&bytes[..end]).into_owned(),
            String::new(),
        ))
    }

    /// Write UTF-8 text, optionally appending and creating parent directories.
    ///
    /// # Errors
    /// Returns an error when parent creation or the write fails.
    pub async fn write_text(
        &self,
        path: &Path,
        content: &str,
        append: bool,
        create_dirs: bool,
    ) -> Result<ProcessResult> {
        let lock = self.lock_for(path);
        let _guard = lock.write().await;
        ensure_parent(path, create_dirs).await?;
        let mut options = OpenOptions::new();
        options.write(true).create(true);
        if append {
            options.append(true);
        } else {
            options.truncate(true);
        }
        let mut file = options
            .open(path)
            .await
            .with_context(|| format!("open {} for writing", path.display()))?;
        file.write_all(content.as_bytes())
            .await
            .with_context(|| format!("write {}", path.display()))?;
        file.flush().await.context("flush text write")?;
        Ok(ProcessResult::success(String::new(), String::new()))
    }

    /// Read an exact byte range with paging metadata, SHA-256, and base64 content.
    ///
    /// # Errors
    /// Returns an error when the target is not a regular readable file.
    pub async fn read_large(
        &self,
        path: &Path,
        offset_bytes: u64,
        max_bytes: usize,
    ) -> Result<LargeReadResult> {
        let lock = self.lock_for(path);
        let _guard = lock.read().await;
        let metadata = fs::metadata(path)
            .await
            .with_context(|| format!("read metadata for {}", path.display()))?;
        if !metadata.is_file() {
            bail!("Not a regular file.");
        }
        let file_size = metadata.len();
        let actual_offset = offset_bytes.min(file_size);
        let bytes_requested = u64::try_from(max_bytes.max(1)).unwrap_or(u64::MAX);
        let remaining = file_size.saturating_sub(actual_offset);
        let bytes_to_read =
            usize::try_from(remaining.min(bytes_requested)).unwrap_or(max_bytes.max(1));

        let mut file = File::open(path)
            .await
            .with_context(|| format!("open {} for reading", path.display()))?;
        file.seek(SeekFrom::Start(actual_offset))
            .await
            .context("seek large read")?;
        let mut chunk = vec![0_u8; bytes_to_read];
        if bytes_to_read > 0 {
            file.read_exact(&mut chunk)
                .await
                .context("read large file chunk")?;
        }
        let bytes_returned = u64::try_from(chunk.len()).unwrap_or(u64::MAX);
        Ok(LargeReadResult {
            path: path.to_string_lossy().into_owned(),
            file_size,
            offset_bytes_requested: offset_bytes,
            offset_bytes: actual_offset,
            bytes_requested,
            bytes_returned,
            next_offset_bytes: actual_offset.saturating_add(bytes_returned),
            eof: actual_offset.saturating_add(bytes_returned) >= file_size,
            content_sha256: sha256_bytes(&chunk),
            content_base64: STANDARD.encode(chunk),
        })
    }

    /// Write exact decoded bytes and verify the resulting file or appended suffix.
    ///
    /// # Errors
    /// Returns an error for invalid base64/SHA input, non-file targets, I/O failures,
    /// or any post-write verification mismatch.
    pub async fn write_large(
        &self,
        path: &Path,
        content_base64: &str,
        append: bool,
        create_dirs: bool,
        expected_sha256: Option<&str>,
    ) -> Result<LargeWriteResult> {
        let normalized_base64 = content_base64
            .chars()
            .filter(|character| !character.is_whitespace())
            .collect::<String>();
        let payload = STANDARD
            .decode(&normalized_base64)
            .context("content_base64 is not valid base64")?;
        if STANDARD.encode(&payload) != normalized_base64 {
            bail!("content_base64 is not canonical base64");
        }
        let content_sha256 = sha256_bytes(&payload);
        let expected = normalize_expected_sha256(expected_sha256)?;
        if let Some(value) = expected.as_deref()
            && value != content_sha256
        {
            bail!("Decoded payload SHA-256 did not match expected_sha256.");
        }

        let lock = self.lock_for(path);
        let _guard = lock.write().await;
        let (target_existed, previous_file_size) = match fs::metadata(path).await {
            Ok(metadata) if metadata.is_file() => (true, metadata.len()),
            Ok(_) => bail!("Target exists but is not a regular file."),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => (false, 0),
            Err(error) => return Err(error).with_context(|| format!("inspect {}", path.display())),
        };
        ensure_parent(path, create_dirs).await?;

        let mut options = OpenOptions::new();
        options.write(true).create(true);
        if append {
            options.append(true);
        } else {
            options.truncate(true);
        }
        let mut file = options
            .open(path)
            .await
            .with_context(|| format!("open {} for exact write", path.display()))?;
        file.write_all(&payload)
            .await
            .context("write exact payload")?;
        file.flush().await.context("flush exact payload")?;
        drop(file);

        let final_metadata = fs::metadata(path)
            .await
            .context("stat exact-write target")?;
        if !final_metadata.is_file() {
            bail!("Target is not a regular file after write.");
        }
        let final_file_size = final_metadata.len();
        let (verification_mode, verified, file_sha256) = if append {
            let verified = verify_suffix(path, previous_file_size, &payload).await?;
            ("suffix-bytes".to_owned(), verified, None)
        } else {
            let file_sha256 = sha256_file(path).await?;
            let verified = final_file_size == u64::try_from(payload.len()).unwrap_or(u64::MAX)
                && file_sha256 == content_sha256;
            ("whole-file-sha256".to_owned(), verified, Some(file_sha256))
        };
        if !verified {
            bail!("Mirror verification failed after writing the payload.");
        }

        Ok(LargeWriteResult {
            path: path.to_string_lossy().into_owned(),
            append,
            previous_file_size,
            final_file_size,
            bytes_written: u64::try_from(payload.len()).unwrap_or(u64::MAX),
            content_sha256,
            verification_mode,
            verified,
            expected_sha256_verified: expected.map(|_| true),
            target_existed,
            file_sha256,
        })
    }

    /// Traverse a host filesystem path using the JS runtime's bounded listing semantics.
    ///
    /// # Errors
    /// Returns an error for non-transient filesystem failures. Missing, permission-denied,
    /// and vanished paths are counted as skipped just like the JS runtime.
    pub async fn list(
        &self,
        options: &ListOptions,
        cancellation: &CancellationToken,
    ) -> Result<ProcessResult> {
        let deadline = Instant::now() + options.timeout.max(Duration::from_millis(1));
        let excluded = options
            .exclude_directories
            .iter()
            .map(|value| value.trim().to_ascii_lowercase())
            .filter(|value| !value.is_empty())
            .collect::<HashSet<_>>();
        let max_entries = options.max_entries.max(1);
        let max_depth = if options.recursive {
            options.max_depth
        } else {
            0
        };
        let mut stack = vec![(options.path.clone(), 0_usize)];
        let mut collected = Vec::new();
        let mut pruned = 0_usize;
        let mut skipped = 0_usize;
        let mut timed_out = false;
        let mut truncated = false;

        while let Some((path, depth)) = stack.pop() {
            if cancellation.is_cancelled() {
                bail!("Recursive filesystem operation cancelled by the MCP client.");
            }
            if Instant::now() >= deadline {
                timed_out = true;
                break;
            }
            let metadata = match fs::symlink_metadata(&path).await {
                Ok(value) => value,
                Err(error) if is_skippable_fs_error(&error) => {
                    skipped = skipped.saturating_add(1);
                    continue;
                }
                Err(error) => {
                    return Err(error).with_context(|| format!("inspect {}", path.display()));
                }
            };
            collected.push(format!("{}\t{}", entry_type(&metadata), path.display()));
            if collected.len() >= max_entries {
                truncated = true;
                break;
            }
            if !metadata.is_dir() || !options.recursive || depth >= max_depth {
                continue;
            }

            let mut reader = match fs::read_dir(&path).await {
                Ok(value) => value,
                Err(error) if is_skippable_fs_error(&error) => {
                    skipped = skipped.saturating_add(1);
                    continue;
                }
                Err(error) => {
                    return Err(error).with_context(|| format!("list {}", path.display()));
                }
            };
            let mut children = Vec::new();
            while let Some(entry) = reader.next_entry().await.context("read directory entry")? {
                children.push(entry.file_name());
            }
            children.sort_by(|left, right| {
                left.to_string_lossy()
                    .to_lowercase()
                    .cmp(&right.to_string_lossy().to_lowercase())
                    .then_with(|| left.cmp(right))
            });
            for child in children.into_iter().rev() {
                let name = child.to_string_lossy();
                if excluded.contains(&name.to_ascii_lowercase()) {
                    pruned = pruned.saturating_add(1);
                    continue;
                }
                stack.push((path.join(child), depth.saturating_add(1)));
            }
        }

        Ok(build_list_result(
            &collected,
            options.timeout,
            max_entries,
            timed_out,
            truncated,
            pruned,
            skipped,
        ))
    }

    fn lock_for(&self, path: &Path) -> Arc<RwLock<()>> {
        let key = absolute_lexical_path(path);
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

fn build_list_result(
    collected: &[String],
    timeout: Duration,
    max_entries: usize,
    timed_out: bool,
    truncated: bool,
    pruned: usize,
    skipped: usize,
) -> ProcessResult {
    let mut notices = Vec::new();
    if timed_out {
        notices.push(format!("listing stopped after {} ms", timeout.as_millis()));
    }
    if truncated {
        notices.push(format!("listing capped at {max_entries} entries"));
    }
    if pruned > 0 {
        notices.push(format!("pruned {pruned} excluded directories"));
    }
    if skipped > 0 {
        notices.push(format!("skipped {skipped} inaccessible or vanished paths"));
    }
    let stdout = if collected.is_empty() {
        String::new()
    } else {
        format!("{}\n", collected.join("\n"))
    };
    let stderr = if notices.is_empty() {
        String::new()
    } else {
        format!("{}\n", notices.join("; "))
    };
    ProcessResult::success(stdout, stderr)
}
fn absolute_lexical_path(path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

async fn ensure_parent(path: &Path, create_dirs: bool) -> Result<()> {
    if create_dirs
        && let Some(parent) = path.parent()
        && !parent.as_os_str().is_empty()
    {
        fs::create_dir_all(parent)
            .await
            .with_context(|| format!("create parent directory {}", parent.display()))?;
    }
    Ok(())
}

fn normalize_expected_sha256(value: Option<&str>) -> Result<Option<String>> {
    let Some(raw) = value.map(str::trim).filter(|value| !value.is_empty()) else {
        return Ok(None);
    };
    if raw.len() != 64 || !raw.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        bail!("expected_sha256 must be a 64-character SHA-256 hex string.");
    }
    Ok(Some(raw.to_ascii_lowercase()))
}

fn sha256_bytes(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

async fn sha256_file(path: &Path) -> Result<String> {
    let mut file = File::open(path).await.context("open file for SHA-256")?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0_u8; 128 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .await
            .context("read file for SHA-256")?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

async fn verify_suffix(path: &Path, previous_file_size: u64, payload: &[u8]) -> Result<bool> {
    if payload.is_empty() {
        return Ok(true);
    }
    let mut file = File::open(path)
        .await
        .context("open append target for verification")?;
    file.seek(SeekFrom::Start(previous_file_size))
        .await
        .context("seek append verification")?;
    let mut actual = vec![0_u8; payload.len()];
    file.read_exact(&mut actual)
        .await
        .context("read append verification suffix")?;
    Ok(actual == payload)
}

fn entry_type(metadata: &std::fs::Metadata) -> char {
    let file_type = metadata.file_type();
    if file_type.is_dir() {
        'd'
    } else if file_type.is_file() {
        'f'
    } else if file_type.is_symlink() {
        'l'
    } else {
        '?'
    }
}

fn is_skippable_fs_error(error: &std::io::Error) -> bool {
    matches!(
        error.kind(),
        std::io::ErrorKind::NotFound | std::io::ErrorKind::PermissionDenied
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn large_read_pages_exact_bytes_and_clamps_eof() {
        let temp = tempfile::tempdir().expect("temp dir");
        let path = temp.path().join("bytes.bin");
        fs::write(&path, b"0123456789")
            .await
            .expect("write fixture");
        let files = FileService::new();
        let result = files.read_large(&path, 7, 8).await.expect("large read");
        assert_eq!(result.file_size, 10);
        assert_eq!(result.offset_bytes, 7);
        assert_eq!(result.bytes_returned, 3);
        assert_eq!(result.next_offset_bytes, 10);
        assert!(result.eof);
        assert_eq!(STANDARD.decode(result.content_base64).unwrap(), b"789");
    }

    #[tokio::test]
    async fn exact_write_verifies_whole_file_and_append_suffix() {
        let temp = tempfile::tempdir().expect("temp dir");
        let path = temp.path().join("mirror.bin");
        let files = FileService::new();
        let first = STANDARD.encode(b"alpha");
        let first_hash = sha256_bytes(b"alpha");
        let result = files
            .write_large(&path, &first, false, true, Some(&first_hash))
            .await
            .expect("initial exact write");
        assert!(result.verified);
        assert_eq!(result.verification_mode, "whole-file-sha256");
        assert_eq!(result.expected_sha256_verified, Some(true));

        let second = STANDARD.encode(b"-beta");
        let append = files
            .write_large(&path, &second, true, true, None)
            .await
            .expect("append exact write");
        assert!(append.verified);
        assert_eq!(append.verification_mode, "suffix-bytes");
        assert_eq!(fs::read(&path).await.unwrap(), b"alpha-beta");
    }

    #[tokio::test]
    async fn exact_write_accepts_whitespace_but_rejects_noncanonical_base64() {
        let temp = tempfile::tempdir().expect("temp dir");
        let path = temp.path().join("base64.bin");
        let files = FileService::new();
        files
            .write_large(&path, "YWxw\naGE=", false, true, None)
            .await
            .expect("whitespace is normalized like the JS implementation");
        assert_eq!(fs::read(&path).await.unwrap(), b"alpha");

        let error = files
            .write_large(&path, "YWxwaGE", false, true, None)
            .await
            .expect_err("missing padding is noncanonical");
        assert!(error.to_string().contains("base64"));
    }

    #[tokio::test]
    async fn listing_is_sorted_pruned_and_bounded() {
        let temp = tempfile::tempdir().expect("temp dir");
        fs::create_dir_all(temp.path().join("node_modules"))
            .await
            .unwrap();
        fs::write(temp.path().join("z.txt"), b"z").await.unwrap();
        fs::write(temp.path().join("a.txt"), b"a").await.unwrap();
        fs::write(temp.path().join("node_modules").join("hidden.txt"), b"x")
            .await
            .unwrap();
        let files = FileService::new();
        let result = files
            .list(
                &ListOptions {
                    path: temp.path().to_path_buf(),
                    recursive: true,
                    max_depth: 4,
                    max_entries: 20,
                    timeout: Duration::from_secs(1),
                    exclude_directories: vec!["node_modules".to_owned()],
                },
                &CancellationToken::new(),
            )
            .await
            .expect("list");
        let lines = result.stdout.lines().collect::<Vec<_>>();
        assert_eq!(lines.len(), 3);
        assert!(lines[1].ends_with("a.txt"));
        assert!(lines[2].ends_with("z.txt"));
        assert!(result.stderr.contains("pruned 1 excluded directories"));
    }
}
