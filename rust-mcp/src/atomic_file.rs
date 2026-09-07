//! Atomic, bounded-memory file replacement and cooperative compare-and-swap.
use anyhow::{Context, Result, bail};
use fs2::FileExt;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::{Arc, OnceLock},
    time::{Duration, Instant},
};

#[derive(Debug, Clone, Default)]
pub(crate) struct Preconditions {
    /// SHA-256 of the previous complete file, or "missing" for create-only.
    pub sha256: Option<String>,
    pub offset: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
pub(crate) struct FileState {
    pub exists: bool,
    pub bytes: u64,
    pub sha256: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub(crate) struct WriteReceipt {
    pub path: String,
    pub previous: FileState,
    pub current: FileState,
    pub replayed: bool,
}

pub(crate) fn digest(bytes: &[u8]) -> String {
    crate::hex::lower_hex(Sha256::digest(bytes))
}

pub(crate) fn state(path: &Path) -> Result<FileState> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(FileState {
                exists: false,
                bytes: 0,
                sha256: None,
            });
        }
        Err(error) => return Err(error.into()),
    };
    if !file.metadata()?.is_file() {
        bail!("Target is not a regular file");
    }
    let initial_len = file.metadata()?.len();
    let mut file = file.take(initial_len.saturating_add(1));
    let mut hash = Sha256::new();
    let mut buffer = vec![0; 65536];
    let mut bytes = 0_u64;
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
        bytes += count as u64;
    }
    if bytes != initial_len {
        bail!("File changed while computing its version");
    }
    Ok(FileState {
        exists: true,
        bytes,
        sha256: Some(crate::hex::lower_hex(hash.finalize())),
    })
}

fn resolved(path: &Path) -> Result<PathBuf> {
    if fs::symlink_metadata(path).is_ok_and(|m| m.file_type().is_symlink()) && !path.exists() {
        bail!("Atomic writes reject dangling symlinks");
    }
    if path.exists() {
        return fs::canonicalize(path).context("resolve atomic target");
    }
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    Ok(fs::canonicalize(parent)?.join(path.file_name().context("file name required")?))
}

pub(crate) fn stripe(path: &Path) -> usize {
    let path = path.to_string_lossy();
    let key = if cfg!(windows) {
        path.to_lowercase()
    } else {
        path.into_owned()
    };
    usize::from(Sha256::digest(key.as_bytes())[0])
}

// Exactly 256 persistent lock files; never unlink a lock held by another process.
fn lock(path: &Path) -> Result<File> {
    let root = std::env::temp_dir().join("devbox-atomic-locks-v1");
    fs::create_dir_all(&root)?;
    let mut options = OpenOptions::new();
    options.create(true).truncate(false).read(true).write(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let file = options.open(root.join(format!("{}.lock", stripe(path))))?;
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        match file.try_lock_exclusive() {
            Ok(()) => break,
            Err(error)
                if error.kind() == std::io::ErrorKind::WouldBlock
                    || error.raw_os_error().is_some_and(|code| {
                        Some(code) == fs2::lock_contended_error().raw_os_error()
                    }) => {}
            Err(error) => return Err(error.into()),
        }
        if Instant::now() >= deadline {
            bail!("Atomic file lock deadline exceeded");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    Ok(file)
}

pub(crate) async fn blocking<T: Send + 'static>(
    operation: impl FnOnce() -> Result<T> + Send + 'static,
) -> Result<T> {
    static WORKERS: OnceLock<Arc<tokio::sync::Semaphore>> = OnceLock::new();
    let permit = tokio::time::timeout(
        Duration::from_secs(5),
        WORKERS
            .get_or_init(|| Arc::new(tokio::sync::Semaphore::new(2)))
            .clone()
            .acquire_owned(),
    )
    .await
    .context("Atomic I/O capacity wait timed out")??;
    // The worker owns the permit even if its HTTP caller disconnects.
    tokio::task::spawn_blocking(move || {
        let _permit = permit;
        operation()
    })
    .await
    .context("join atomic I/O")?
}

pub(crate) async fn write(
    path: PathBuf,
    payload: Vec<u8>,
    append: bool,
    create_dirs: bool,
    expected: Preconditions,
) -> Result<WriteReceipt> {
    blocking(move || write_sync(&path, &payload, append, create_dirs, &expected)).await
}

pub(crate) fn write_sync(
    path: &Path,
    payload: &[u8],
    append: bool,
    create_dirs: bool,
    expected: &Preconditions,
) -> Result<WriteReceipt> {
    if create_dirs && let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let target = resolved(path)?;
    let _lock = lock(&target)?;
    let previous = state(&target)?;
    if let Some(expected_hash) = &expected.sha256 {
        if expected_hash != "missing"
            && (expected_hash.len() != 64 || !expected_hash.bytes().all(|c| c.is_ascii_hexdigit()))
        {
            bail!("expected_file_sha256 must be a SHA-256 digest or missing");
        }
        let matches = if expected_hash == "missing" {
            !previous.exists
        } else {
            previous.sha256.as_deref() == Some(expected_hash.to_ascii_lowercase().as_str())
        };
        if !(matches || append && append_replayed(&target, payload, expected)?) {
            // An identical overwrite already fulfils the requested state after a lost response.
            if !append && previous.sha256.as_deref() == Some(digest(payload).as_str()) {
                return Ok(WriteReceipt {
                    path: path.to_string_lossy().into_owned(),
                    current: previous.clone(),
                    previous,
                    replayed: true,
                });
            }
            bail!("File version conflict: current content differs from expected_file_sha256");
        }
        if !matches {
            return Ok(WriteReceipt {
                path: path.to_string_lossy().into_owned(),
                current: previous.clone(),
                previous,
                replayed: true,
            });
        }
    }
    if let Some(offset) = expected.offset {
        if !append {
            bail!("expected_offset_bytes requires append=true");
        }
        if previous.bytes != offset {
            if append_replayed(&target, payload, expected)? {
                return Ok(WriteReceipt {
                    path: path.to_string_lossy().into_owned(),
                    current: previous.clone(),
                    previous,
                    replayed: true,
                });
            }
            bail!("File offset conflict: append was not applied");
        }
    }
    let metadata = fs::metadata(&target).ok();
    #[cfg(windows)]
    if previous.exists {
        reject_windows_hard_links(&target)?;
    }
    if let Some(metadata) = &metadata
        && metadata.permissions().readonly()
    {
        bail!("Target is read-only");
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        if metadata.nlink() > 1 {
            bail!("Atomic replacement of a hard-linked target is unsupported");
        }
    }
    let staged = tempfile::Builder::new()
        .prefix(".devbox-write-")
        .tempfile_in(target.parent().context("target parent")?)?;
    let (mut file, staged_path) = staged.into_parts();
    if append && previous.exists {
        std::io::copy(&mut File::open(&target)?.take(previous.bytes), &mut file)?;
    }
    file.write_all(payload)?;
    if let Some(metadata) = metadata {
        file.set_permissions(metadata.permissions())?;
    }
    file.sync_all()?;
    drop(file);
    let current = state(&staged_path)?;
    // Detect non-cooperating writers during staging; cooperative writers hold the OS lock.
    let before_commit = state(&target)?;
    if before_commit.sha256 != previous.sha256 {
        bail!("File changed while preparing atomic replacement");
    }
    replace(&staged_path, &target)?;
    #[cfg(unix)]
    {
        File::open(target.parent().context("target parent")?)?.sync_all()?;
    }
    Ok(WriteReceipt {
        path: path.to_string_lossy().into_owned(),
        previous,
        current,
        replayed: false,
    })
}

fn append_replayed(path: &Path, payload: &[u8], expected: &Preconditions) -> Result<bool> {
    use std::io::{Seek, SeekFrom};
    let Some(offset) = expected.offset else {
        return Ok(false);
    };
    let Ok(mut file) = File::open(path) else {
        return Ok(false);
    };
    if file.metadata()?.len() != offset.saturating_add(payload.len() as u64) {
        return Ok(false);
    }
    if let Some(hash) = &expected.sha256 {
        if hash == "missing" {
            if offset != 0 {
                return Ok(false);
            }
        } else {
            let mut prefix = Sha256::new();
            let mut remaining = offset;
            let mut buffer = vec![0; 65536];
            while remaining > 0 {
                let limit = usize::try_from(remaining.min(65536))?;
                let count = file.read(&mut buffer[..limit])?;
                if count == 0 {
                    return Ok(false);
                }
                prefix.update(&buffer[..count]);
                remaining -= count as u64;
            }
            if crate::hex::lower_hex(prefix.finalize()) != hash.to_ascii_lowercase() {
                return Ok(false);
            }
        }
    }
    file.seek(SeekFrom::Start(offset))?;
    let mut suffix = Vec::new();
    file.take(payload.len() as u64 + 1)
        .read_to_end(&mut suffix)?;
    Ok(suffix == payload)
}

#[cfg(not(windows))]
fn replace(source: &Path, target: &Path) -> Result<()> {
    fs::rename(source, target).context("atomic file replacement")
}

#[cfg(windows)]
#[allow(
    unsafe_code,
    reason = "native file information checks hard-link count before replacement"
)]
fn reject_windows_hard_links(path: &Path) -> Result<()> {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Storage::FileSystem::{
        BY_HANDLE_FILE_INFORMATION, GetFileInformationByHandle,
    };
    let file = File::open(path)?;
    let mut info = std::mem::MaybeUninit::<BY_HANDLE_FILE_INFORMATION>::zeroed();
    if unsafe { GetFileInformationByHandle(file.as_raw_handle(), info.as_mut_ptr()) } == 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    if unsafe { info.assume_init() }.nNumberOfLinks > 1 {
        bail!("Atomic replacement of a hard-linked target is unsupported");
    }
    Ok(())
}

#[cfg(windows)]
#[allow(
    unsafe_code,
    reason = "ReplaceFileW preserves the destination ACL while replacing atomically"
)]
fn replace(source: &Path, target: &Path) -> Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::{
        Foundation::GetLastError,
        Storage::FileSystem::{MOVEFILE_WRITE_THROUGH, MoveFileExW, ReplaceFileW},
    };
    let source: Vec<u16> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let target_wide: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
    let success = if target.exists() {
        unsafe {
            ReplaceFileW(
                target_wide.as_ptr(),
                source.as_ptr(),
                std::ptr::null(),
                0,
                std::ptr::null(),
                std::ptr::null(),
            )
        }
    } else {
        unsafe {
            MoveFileExW(
                source.as_ptr(),
                target_wide.as_ptr(),
                MOVEFILE_WRITE_THROUGH,
            )
        }
    };
    if success == 0 {
        return Err(std::io::Error::from_raw_os_error(
            i32::try_from(unsafe { GetLastError() }).unwrap_or(i32::MAX),
        )
        .into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(windows)]
    #[tokio::test]
    async fn failed_commit_after_staging_keeps_the_previous_file() {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_READ, FILE_SHARE_WRITE};
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("checkpoint");
        fs::write(&path, b"old checkpoint").unwrap();
        let blocker = OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
            .open(&path)
            .unwrap();
        let error = write(
            path.clone(),
            vec![b'x'; 1024 * 1024],
            false,
            false,
            Preconditions::default(),
        )
        .await
        .unwrap_err();
        assert!(!error.to_string().is_empty());
        assert_eq!(fs::read(&path).unwrap(), b"old checkpoint");
        assert_eq!(
            fs::read_dir(root.path()).unwrap().count(),
            1,
            "failed staging must not strand a temporary file"
        );
        drop(blocker);
        write(
            path.clone(),
            b"new checkpoint".to_vec(),
            false,
            false,
            Preconditions::default(),
        )
        .await
        .unwrap();
        assert_eq!(fs::read(path).unwrap(), b"new checkpoint");
    }

    #[tokio::test]
    async fn hard_link_aliases_are_preserved_on_rejected_replacement() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("original");
        let alias = dir.path().join("alias");
        fs::write(&path, b"original").unwrap();
        fs::hard_link(&path, &alias).unwrap();
        let error = write(
            path.clone(),
            b"new".to_vec(),
            false,
            false,
            Preconditions::default(),
        )
        .await
        .unwrap_err();
        assert!(error.to_string().contains("hard-linked"));
        assert_eq!(fs::read(&path).unwrap(), b"original");
        assert_eq!(fs::read(&alias).unwrap(), b"original");
    }
    #[tokio::test]
    async fn replacement_and_append_retries_preserve_exact_state() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("checkpoint");
        let create = Preconditions {
            sha256: Some("missing".into()),
            offset: None,
        };
        write(path.clone(), b"old".to_vec(), false, false, create.clone())
            .await
            .unwrap();
        assert!(
            write(path.clone(), b"different".to_vec(), false, false, create)
                .await
                .is_err()
        );
        assert_eq!(fs::read(&path).unwrap(), b"old");
        let expected = Preconditions {
            sha256: Some(digest(b"old")),
            offset: Some(3),
        };
        let first = write(path.clone(), b"new".to_vec(), true, false, expected.clone())
            .await
            .unwrap();
        assert!(!first.replayed);
        assert!(
            write(path.clone(), b"new".to_vec(), true, false, expected)
                .await
                .unwrap()
                .replayed
        );
        assert_eq!(fs::read(&path).unwrap(), b"oldnew");
    }
    #[tokio::test]
    async fn concurrent_compare_and_swap_has_one_winner() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("checkpoint");
        fs::write(&path, b"old").unwrap();
        let expected = Preconditions {
            sha256: Some(digest(b"old")),
            offset: None,
        };
        let (a, b) = tokio::join!(
            write(
                path.clone(),
                b"first".to_vec(),
                false,
                false,
                expected.clone()
            ),
            write(path.clone(), b"second".to_vec(), false, false, expected)
        );
        assert_ne!(a.is_ok(), b.is_ok());
        let bytes = fs::read(path).unwrap();
        assert!(bytes == b"first" || bytes == b"second");
    }
}
