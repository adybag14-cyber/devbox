use std::{
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, Instant},
};

use serde_json::{Value, json};
use tokio::sync::RwLock;
use tokio_util::sync::CancellationToken;

use crate::{
    background::BackgroundTaskRegistry, performance::PerformanceMonitor,
    request_control::ActiveRequestRegistry, usage::ActiveToolRegistry,
};

const INCIDENT_INTERVAL: Duration = Duration::from_secs(10);
const INCIDENT_COOLDOWN: Duration = Duration::from_secs(30);
const INCIDENT_MAX_BYTES: u64 = 4 * 1024 * 1024;
const INCIDENT_ROTATIONS: usize = 3;

#[derive(Clone)]
pub struct IncidentMonitorContext {
    pub performance: Arc<PerformanceMonitor>,
    pub execution_snapshot: Arc<RwLock<Value>>,
    pub active_requests: Arc<ActiveRequestRegistry>,
    pub active_tools: Arc<ActiveToolRegistry>,
    pub background: Arc<BackgroundTaskRegistry>,
    pub execution_store: Arc<RwLock<Value>>,
}

pub fn spawn_incident_monitor(
    project_root: PathBuf,
    context: IncidentMonitorContext,
    cancellation: CancellationToken,
) {
    let IncidentMonitorContext {
        performance,
        execution_snapshot,
        active_requests,
        active_tools,
        background,
        execution_store,
    } = context;
    let registry = background.clone();
    registry.spawn_supervised("incident-monitor", cancellation, move |cancellation, heartbeat| {
        let path = project_root.join("run").join("mcp-incidents.jsonl");
        let performance = performance.clone();
        let execution_snapshot = execution_snapshot.clone();
        let active_requests = active_requests.clone();
        let active_tools = active_tools.clone();
        let background = background.clone();
        let execution_store = execution_store.clone();
        async move {
            let first_tick = tokio::time::Instant::now() + Duration::from_secs(7);
            let mut interval = tokio::time::interval_at(first_tick, INCIDENT_INTERVAL);
            interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            let mut last_incident = None::<Instant>;
            loop {
                tokio::select! {
                    () = cancellation.cancelled() => return Ok(()),
                    _ = interval.tick() => {
                        heartbeat.attempt();
                        let snapshot = performance.snapshot();
                        let p95 = number(&snapshot, "/eventLoop/p95Ms");
                        let max = number(&snapshot, "/eventLoop/maxMs");
                        let drift = number(&snapshot, "/eventLoop/timerDriftMaxMs");
                        let anomalous = is_anomalous(&snapshot);
                        let cooled = last_incident.is_none_or(|at| at.elapsed() >= INCIDENT_COOLDOWN);
                        if anomalous && cooled {
                            let execution_cache = execution_snapshot.read().await.clone();
                            let execution = execution_cache.get("snapshot").cloned().unwrap_or(Value::Null);
                            let incident = json!({
                                "observedAtUtc": chrono::Utc::now().to_rfc3339(),
                                "trigger": { "p95Ms": p95, "maxMs": max, "timerDriftMaxMs": drift },
                                "performance": snapshot,
                                "build": crate::provenance::snapshot(),
                                "execution": execution,
                                "executionSnapshot": {
                                    "sampledAtUtc": execution_cache.get("sampledAtUtc"),
                                    "ok": execution_cache.get("ok"),
                                    "error": execution_cache.get("error"),
                                },
                                "activeRequests": active_requests.active_count(),
                                "activeTools": active_tools.snapshot(),
                                "backgroundTasks": background.snapshot(),
                                "executionStore": execution_store.read().await.clone(),
                            });
                            match append_rotating_jsonl(&path, &incident).await {
                                Ok(()) => last_incident = Some(Instant::now()),
                                Err(error) => heartbeat.fail(format!("incident write failed: {error}")),
                            }
                        }
                        heartbeat.tick();
                    }
                }
            }
        }
    });
}

fn is_anomalous(snapshot: &Value) -> bool {
    number(snapshot, "/eventLoop/p95Ms") > 100.0
        || number(snapshot, "/eventLoop/maxMs") > 500.0
        || number(snapshot, "/eventLoop/timerDriftMaxMs") > 250.0
}

fn number(value: &Value, pointer: &str) -> f64 {
    value
        .pointer(pointer)
        .and_then(Value::as_f64)
        .unwrap_or(0.0)
}

async fn append_rotating_jsonl(path: &Path, value: &Value) -> std::io::Result<()> {
    use tokio::io::AsyncWriteExt as _;
    if let Some(parent) = path.parent() {
        tokio::fs::create_dir_all(parent).await?;
    }
    if tokio::fs::metadata(path)
        .await
        .map(|metadata| metadata.len())
        .unwrap_or(0)
        >= INCIDENT_MAX_BYTES
    {
        let oldest = rotation_path(path, INCIDENT_ROTATIONS);
        tokio::fs::remove_file(&oldest).await.ok();
        for index in (1..INCIDENT_ROTATIONS).rev() {
            let from = rotation_path(path, index);
            let to = rotation_path(path, index + 1);
            let _ = tokio::fs::rename(from, to).await;
        }
        let _ = tokio::fs::rename(path, rotation_path(path, 1)).await;
    }
    let mut bytes = serde_json::to_vec(value).map_err(std::io::Error::other)?;
    bytes.push(b'\n');
    let mut file = tokio::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .await?;
    file.write_all(&bytes).await
}

fn rotation_path(path: &Path, index: usize) -> PathBuf {
    PathBuf::from(format!("{}.{index}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn incident_thresholds_match_production_policy() {
        let normal = json!({"eventLoop":{"p95Ms":99.9,"maxMs":499.9,"timerDriftMaxMs":249.9}});
        assert!(!is_anomalous(&normal));
        for snapshot in [
            json!({"eventLoop":{"p95Ms":100.1,"maxMs":0.0,"timerDriftMaxMs":0.0}}),
            json!({"eventLoop":{"p95Ms":0.0,"maxMs":500.1,"timerDriftMaxMs":0.0}}),
            json!({"eventLoop":{"p95Ms":0.0,"maxMs":0.0,"timerDriftMaxMs":250.1}}),
        ] {
            assert!(is_anomalous(&snapshot));
        }
    }
}
