use std::{
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use tokio::{fs, sync::Semaphore};
use tokio_util::sync::CancellationToken;

use crate::{
    Config,
    process::{ProcessError, ProcessOptions, spawn_process},
};

const RETRY_BACKOFF: Duration = Duration::from_millis(150);
static UNIQUE_COUNTER: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone)]
pub struct CaptureResult {
    pub image: Vec<u8>,
    pub mime_type: String,
    pub metadata: Value,
}

#[derive(Debug, Clone)]
pub struct CaptureService {
    config: Arc<Config>,
    gate: Arc<Semaphore>,
}

impl CaptureService {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        Self {
            config,
            gate: Arc::new(Semaphore::new(1)),
        }
    }

    /// Capture the host virtual display using the bounded native capture worker.
    ///
    /// # Errors
    /// Returns validation, queue timeout, cancellation, worker, image-validation, or I/O errors.
    pub async fn display(
        &self,
        quality: u8,
        cancellation: CancellationToken,
    ) -> Result<CaptureResult> {
        validate_quality(quality)?;
        self.capture_with_policy(
            "display",
            vec!["display".to_owned(), quality.to_string()],
            cancellation,
        )
        .await
    }

    /// Capture the largest visible top-level window for a process/process tree.
    ///
    /// # Errors
    /// Returns validation, queue timeout, cancellation, worker, image-validation, or I/O errors.
    pub async fn program(
        &self,
        pid: u32,
        quality: u8,
        include_process_tree: bool,
        cancellation: CancellationToken,
    ) -> Result<CaptureResult> {
        if pid == 0 {
            bail!("pid must be a positive process ID.");
        }
        validate_quality(quality)?;
        self.capture_with_policy(
            "program",
            vec![
                "program".to_owned(),
                quality.to_string(),
                pid.to_string(),
                include_process_tree.to_string(),
            ],
            cancellation,
        )
        .await
    }

    async fn capture_with_policy(
        &self,
        operation: &str,
        worker_args: Vec<String>,
        cancellation: CancellationToken,
    ) -> Result<CaptureResult> {
        let queue_started = Instant::now();
        let queue_timeout =
            Duration::from_millis(self.config.screen_capture_queue_timeout_ms.max(1));
        let permit = tokio::select! {
            biased;
            () = cancellation.cancelled() => bail!("Screen capture cancelled while waiting for the capture worker."),
            result = tokio::time::timeout(queue_timeout, self.gate.acquire()) => {
                match result {
                    Ok(Ok(permit)) => permit,
                    Ok(Err(_)) => bail!("Screen capture queue was closed."),
                    Err(_) => bail!("Screen capture queue remained busy for {} ms. Retry shortly.", queue_timeout.as_millis()),
                }
            }
        };
        let queue_wait_ms = duration_ms(queue_started.elapsed());
        let attempts = self.config.screen_capture_retries.saturating_add(1).max(1);
        let attempt_timeout =
            Duration::from_millis(self.config.screen_capture_attempt_timeout_ms.max(1));
        let overall_timeout_ms =
            duration_ms(
                attempt_timeout
                    .saturating_mul(u32::try_from(attempts).unwrap_or(u32::MAX))
                    .saturating_add(RETRY_BACKOFF.saturating_mul(
                        u32::try_from(attempts.saturating_sub(1)).unwrap_or(u32::MAX),
                    )),
            );
        let mut last_error = None;
        let mut completed_attempt = 0_usize;

        for attempt in 1..=attempts {
            completed_attempt = attempt;
            match self
                .worker_attempt(&worker_args, attempt_timeout, cancellation.child_token())
                .await
            {
                Ok(mut capture) => {
                    let metadata = capture.metadata.as_object_mut().with_context(|| {
                        format!("{operation} capture worker metadata must be a JSON object")
                    })?;
                    metadata.insert("capture_attempts".to_owned(), json!(attempt));
                    metadata.insert("capture_retried".to_owned(), json!(attempt > 1));
                    metadata.insert("capture_queue_wait_ms".to_owned(), json!(queue_wait_ms));
                    metadata.insert(
                        "capture_attempt_timeout_ms".to_owned(),
                        json!(duration_ms(attempt_timeout)),
                    );
                    metadata.insert(
                        "capture_overall_timeout_ms".to_owned(),
                        json!(overall_timeout_ms),
                    );
                    drop(permit);
                    return Ok(capture);
                }
                Err(error) => {
                    let transient = is_transient_capture_error(&error);
                    last_error = Some(error);
                    if attempt >= attempts || !transient {
                        break;
                    }
                    tokio::select! {
                        () = cancellation.cancelled() => bail!("Screen capture cancelled before retry."),
                        () = tokio::time::sleep(RETRY_BACKOFF) => {},
                    }
                }
            }
        }
        drop(permit);
        let mut error = last_error.unwrap_or_else(|| anyhow::anyhow!("Screen capture failed."));
        error = error.context(format!(
            "Screen capture failed after {completed_attempt} attempt(s); queue_wait_ms={queue_wait_ms}; attempt_timeout_ms={}; overall_timeout_ms={overall_timeout_ms}",
            duration_ms(attempt_timeout)
        ));
        Err(error)
    }

    async fn worker_attempt(
        &self,
        worker_args: &[String],
        timeout: Duration,
        cancellation: CancellationToken,
    ) -> Result<CaptureResult> {
        let executable =
            std::env::current_exe().context("resolve Rust MCP executable for capture worker")?;
        let temp_dir = temporary_capture_dir();
        fs::create_dir_all(&temp_dir).await?;
        let image_path = temp_dir.join("capture.bin");
        let mut args = vec![
            "--capture-worker".to_owned(),
            image_path.to_string_lossy().into_owned(),
        ];
        args.extend(worker_args.iter().cloned());
        let result = spawn_process(
            &executable.to_string_lossy(),
            &args,
            ProcessOptions {
                timeout: Some(timeout),
                max_capture_chars: Some(200_000),
                ..ProcessOptions::default()
            },
            cancellation,
        )
        .await;
        let outcome = async {
            let process = result.map_err(anyhow::Error::new)?;
            let mut metadata: Value = serde_json::from_str(process.stdout.trim())
                .context("parse Rust capture-worker metadata")?;
            let image = fs::read(&image_path)
                .await
                .with_context(|| format!("read capture image {}", image_path.display()))?;
            let mime_type = metadata["mime_type"]
                .as_str()
                .map(str::to_owned)
                .context("capture-worker metadata omitted mime_type")?;
            validate_image(&image, &mime_type)?;
            let object = metadata
                .as_object_mut()
                .context("capture-worker metadata must be a JSON object")?;
            object.insert("bytes".to_owned(), json!(image.len()));
            object.insert("sha256".to_owned(), Value::String(sha256_hex(&image)));
            Ok(CaptureResult {
                image,
                mime_type,
                metadata,
            })
        }
        .await;
        fs::remove_dir_all(&temp_dir).await.ok();
        outcome
    }
}

/// Hidden worker entry point used by the long-lived MCP to make capture attempts killable.
///
/// Arguments are `<output-path> display <quality>` or
/// `<output-path> program <quality> <pid> <include-process-tree>`.
///
/// # Errors
/// Returns argument, platform, capture, image-write, or metadata serialization errors.
pub async fn run_capture_worker(arguments: &[String]) -> Result<()> {
    let output_path = arguments
        .first()
        .map(PathBuf::from)
        .context("capture worker requires an output path")?;
    let mode = arguments.get(1).map(String::as_str).unwrap_or_default();
    let quality = arguments
        .get(2)
        .context("capture worker requires quality")?
        .parse::<u8>()
        .context("parse capture quality")?;
    validate_quality(quality)?;

    #[cfg(windows)]
    let capture = match mode {
        "display" => crate::windows_capture::capture_display(quality)?,
        "program" => {
            let pid = arguments
                .get(3)
                .context("program capture requires pid")?
                .parse::<u32>()
                .context("parse capture pid")?;
            let include_process_tree = arguments
                .get(4)
                .context("program capture requires include-process-tree")?
                .parse::<bool>()
                .context("parse include-process-tree")?;
            crate::windows_capture::capture_program(pid, quality, include_process_tree)?
        }
        other => bail!("unsupported capture-worker mode {other:?}"),
    };

    #[cfg(not(windows))]
    let capture = match mode {
        "display" => crate::posix_capture::capture_display(quality).await?,
        "program" => {
            let pid = arguments
                .get(3)
                .context("program capture requires pid")?
                .parse::<u32>()
                .context("parse capture pid")?;
            let include_process_tree = arguments
                .get(4)
                .context("program capture requires include-process-tree")?
                .parse::<bool>()
                .context("parse include-process-tree")?;
            crate::posix_capture::capture_program(pid, quality, include_process_tree).await?
        }
        other => bail!("unsupported capture-worker mode {other:?}"),
    };

    if let Some(parent) = output_path.parent() {
        fs::create_dir_all(parent).await?;
    }
    fs::write(&output_path, &capture.image)
        .await
        .with_context(|| format!("write capture image {}", output_path.display()))?;
    let mut metadata = capture.metadata;
    let object = metadata
        .as_object_mut()
        .context("native capture metadata must be an object")?;
    object.insert(
        "mime_type".to_owned(),
        Value::String(capture.mime_type.to_owned()),
    );
    println!("{}", serde_json::to_string(&metadata)?);
    Ok(())
}

fn validate_quality(quality: u8) -> Result<()> {
    if !(1..=100).contains(&quality) {
        bail!("quality must be between 1 and 100.");
    }
    Ok(())
}

fn validate_image(image: &[u8], mime_type: &str) -> Result<()> {
    match mime_type {
        "image/jpeg" => {
            if image.len() < 5
                || !image.starts_with(&[0xff, 0xd8, 0xff])
                || !image.ends_with(&[0xff, 0xd9])
            {
                bail!("Screen capture did not return a valid JPEG image.");
            }
        }
        "image/png" => {
            const PNG_SIGNATURE: &[u8] = &[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
            if !image.starts_with(PNG_SIGNATURE) {
                bail!("Screen capture did not return a valid PNG image.");
            }
        }
        other => bail!("Screen capture returned unsupported MIME type {other:?}."),
    }
    Ok(())
}

fn is_transient_capture_error(error: &anyhow::Error) -> bool {
    if let Some(process) = error.downcast_ref::<ProcessError>()
        && process.timed_out
    {
        return true;
    }
    let message = error.to_string().to_ascii_lowercase();
    [
        "timed out",
        "timeout",
        "printwindow",
        "bitblt",
        "getdc",
        "desktop compositor",
        "capture did not return",
        "capture-worker metadata",
    ]
    .iter()
    .any(|needle| message.contains(needle))
}

fn temporary_capture_dir() -> PathBuf {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_millis());
    let counter = UNIQUE_COUNTER.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "devbox-rust-capture-{millis}-{}-{counter}",
        std::process::id()
    ))
}

fn sha256_hex(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn duration_ms(duration: Duration) -> u64 {
    u64::try_from(duration.as_millis()).unwrap_or(u64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn image_validation_rejects_wrong_magic() {
        assert!(validate_image(b"not jpeg", "image/jpeg").is_err());
        assert!(validate_image(b"not png", "image/png").is_err());
    }

    #[test]
    fn capture_retry_backoff_matches_javascript_policy() {
        assert_eq!(RETRY_BACKOFF, Duration::from_millis(150));
    }

    #[test]
    fn transient_classifier_distinguishes_window_discovery_from_capture_stalls() {
        assert!(is_transient_capture_error(&anyhow::anyhow!(
            "BitBlt failed"
        )));
        assert!(is_transient_capture_error(&anyhow::anyhow!(
            "capture timed out"
        )));
        assert!(!is_transient_capture_error(&anyhow::anyhow!(
            "process has no visible window"
        )));
    }
}
