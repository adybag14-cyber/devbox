use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

use serde_json::json;
use tokio_util::sync::CancellationToken;

use crate::{background::BackgroundTaskRegistry, jobs::JobStore};

pub fn spawn_job_quota(
    store: JobStore,
    state_path: PathBuf,
    background: &BackgroundTaskRegistry,
    cancellation: CancellationToken,
) {
    background.spawn_supervised("job-quota", cancellation, move |cancellation, heartbeat| {
        let store = store.clone();
        let state_path = state_path.clone();
        async move {
            let mut interval = tokio::time::interval(Duration::from_secs(60));
            interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            loop {
                tokio::select! {
                    () = cancellation.cancelled() => return Ok(()),
                    _ = interval.tick() => {
                        heartbeat.attempt();
                        let started = Instant::now();
                        match store.enforce_store_quota().await {
                            Ok(summary) => {
                                let value = json!({
                                    "sampledAtUtc": chrono::Utc::now().to_rfc3339(),
                                    "durationMs": u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
                                    "summary": summary,
                                });
                                match write_json_snapshot(&state_path, &value).await {
                                    Ok(()) => heartbeat.tick(),
                                    Err(error) => heartbeat.fail(error.to_string()),
                                }
                            }
                            Err(error) => {
                                heartbeat.fail(error.to_string());
                                let value = json!({
                                    "sampledAtUtc": chrono::Utc::now().to_rfc3339(),
                                    "durationMs": u64::try_from(started.elapsed().as_millis()).unwrap_or(u64::MAX),
                                    "error": error.to_string(),
                                });
                                let _ = write_json_snapshot(&state_path, &value).await;
                            }
                        }
                    }
                }
            }
        }
    });
}

async fn write_json_snapshot(
    path: &std::path::Path,
    value: &serde_json::Value,
) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        tokio::fs::create_dir_all(parent).await?;
    }
    let bytes = serde_json::to_vec_pretty(value).map_err(std::io::Error::other)?;
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    tokio::fs::write(&temporary, bytes).await?;
    if tokio::fs::rename(&temporary, path).await.is_err() {
        tokio::fs::remove_file(path).await.ok();
        tokio::fs::rename(&temporary, path).await?;
    }
    Ok(())
}
