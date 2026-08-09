use std::{
    collections::VecDeque,
    path::PathBuf,
    sync::{Arc, Mutex, MutexGuard},
    time::{Duration, Instant},
};

use chrono::{SecondsFormat, Utc};
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

const SAMPLE_INTERVAL: Duration = Duration::from_millis(20);
const DRIFT_INTERVAL: Duration = Duration::from_secs(1);
const PERSIST_INTERVAL: Duration = Duration::from_secs(10);
const MAX_SAMPLES: usize = 3_000;

#[derive(Debug, Default)]
struct PerformanceWindow {
    delays_ms: VecDeque<f64>,
    timer_drift_max_ms: f64,
}

#[derive(Debug)]
struct PerformanceInner {
    state: Arc<Mutex<PerformanceWindow>>,
    started_at: Instant,
    cancellation: CancellationToken,
}

impl Drop for PerformanceInner {
    fn drop(&mut self) {
        self.cancellation.cancel();
    }
}

#[derive(Debug, Clone)]
pub struct PerformanceMonitor {
    inner: Arc<PerformanceInner>,
}

impl PerformanceMonitor {
    #[must_use]
    pub fn new(state_path: PathBuf) -> Self {
        let inner = Arc::new(PerformanceInner {
            state: Arc::new(Mutex::new(PerformanceWindow::default())),
            started_at: Instant::now(),
            cancellation: CancellationToken::new(),
        });
        spawn_sampler(&inner);
        spawn_persistence(&inner, state_path);
        Self { inner }
    }

    #[must_use]
    pub fn snapshot(&self) -> Value {
        snapshot_value(&self.inner.state, self.inner.started_at)
    }
}

fn spawn_sampler(inner: &PerformanceInner) {
    let state = inner.state.clone();
    let cancellation = inner.cancellation.child_token();
    tokio::spawn(async move {
        let mut expected_sample = tokio::time::Instant::now() + SAMPLE_INTERVAL;
        let mut expected_drift = tokio::time::Instant::now() + DRIFT_INTERVAL;
        loop {
            tokio::select! {
                () = cancellation.cancelled() => return,
                () = tokio::time::sleep_until(expected_sample) => {
                    let now = tokio::time::Instant::now();
                    let delay_ms = now.saturating_duration_since(expected_sample).as_secs_f64() * 1_000.0;
                    let mut window = lock_window(&state);
                    window.delays_ms.push_back(delay_ms);
                    while window.delays_ms.len() > MAX_SAMPLES {
                        window.delays_ms.pop_front();
                    }
                    if now >= expected_drift {
                        let drift_ms = now.saturating_duration_since(expected_drift).as_secs_f64() * 1_000.0;
                        window.timer_drift_max_ms = window.timer_drift_max_ms.max(drift_ms);
                        expected_drift = now + DRIFT_INTERVAL;
                    }
                    drop(window);
                    expected_sample += SAMPLE_INTERVAL;
                    if now.saturating_duration_since(expected_sample) > Duration::from_secs(1) {
                        expected_sample = now + SAMPLE_INTERVAL;
                    }
                }
            }
        }
    });
}

fn spawn_persistence(inner: &PerformanceInner, state_path: PathBuf) {
    let state = inner.state.clone();
    let started_at = inner.started_at;
    let cancellation = inner.cancellation.child_token();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(PERSIST_INTERVAL);
        interval.tick().await;
        loop {
            tokio::select! {
                () = cancellation.cancelled() => return,
                _ = interval.tick() => {
                    let snapshot = snapshot_value(&state, started_at);
                    if let Err(error) = persist_snapshot(&state_path, &snapshot).await {
                        tracing::debug!(%error, path = %state_path.display(), "failed to persist Rust MCP performance snapshot");
                    }
                    let mut window = lock_window(&state);
                    window.delays_ms.clear();
                    window.timer_drift_max_ms = 0.0;
                }
            }
        }
    });
}

async fn persist_snapshot(path: &std::path::Path, snapshot: &Value) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        tokio::fs::create_dir_all(parent).await?;
    }
    let bytes = serde_json::to_vec_pretty(snapshot).map_err(std::io::Error::other)?;
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    tokio::fs::write(&temporary, bytes).await?;
    if tokio::fs::rename(&temporary, path).await.is_err() {
        tokio::fs::remove_file(path).await.ok();
        tokio::fs::rename(&temporary, path).await?;
    }
    Ok(())
}

fn snapshot_value(state: &Arc<Mutex<PerformanceWindow>>, started_at: Instant) -> Value {
    let window = lock_window(state);
    let mut delays = window.delays_ms.iter().copied().collect::<Vec<_>>();
    let timer_drift_max_ms = window.timer_drift_max_ms;
    drop(window);
    delays.sort_by(f64::total_cmp);
    json!({
        "eventLoop": {
            "sampledAtUtc": Utc::now().to_rfc3339_opts(SecondsFormat::Millis, true),
            "p50Ms": percentile(&delays, 50),
            "p95Ms": percentile(&delays, 95),
            "p99Ms": percentile(&delays, 99),
            "maxMs": delays.last().copied().unwrap_or(0.0),
            "timerDriftMaxMs": timer_drift_max_ms,
        },
        "process": {
            "pid": std::process::id(),
            "uptimeSeconds": started_at.elapsed().as_secs_f64(),
            "memory": process_memory_snapshot(),
        },
    })
}

fn percentile(sorted: &[f64], percent: usize) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let last = sorted.len().saturating_sub(1);
    let index = last.saturating_mul(percent.min(100)).saturating_add(50) / 100;
    sorted[index.min(last)]
}

fn lock_window(state: &Arc<Mutex<PerformanceWindow>>) -> MutexGuard<'_, PerformanceWindow> {
    state
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn process_memory_snapshot() -> Value {
    json!({
        "rss": resident_memory_bytes(),
        "heapTotal": 0,
        "heapUsed": 0,
        "external": 0,
        "arrayBuffers": 0,
    })
}

#[cfg(target_os = "linux")]
fn resident_memory_bytes() -> u64 {
    let Ok(status) = std::fs::read_to_string("/proc/self/status") else {
        return 0;
    };
    status
        .lines()
        .find_map(|line| line.strip_prefix("VmRSS:"))
        .and_then(|value| value.split_whitespace().next())
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0)
        .saturating_mul(1_024)
}

#[cfg(windows)]
fn resident_memory_bytes() -> u64 {
    windows_memory::resident_memory_bytes()
}

#[cfg(not(any(windows, target_os = "linux")))]
const fn resident_memory_bytes() -> u64 {
    0
}

#[cfg(windows)]
mod windows_memory {
    #![allow(unsafe_code)]

    use std::mem::{size_of, zeroed};
    use windows_sys::Win32::System::{
        ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS},
        Threading::GetCurrentProcess,
    };

    pub fn resident_memory_bytes() -> u64 {
        unsafe {
            let mut counters: PROCESS_MEMORY_COUNTERS = zeroed();
            counters.cb = u32::try_from(size_of::<PROCESS_MEMORY_COUNTERS>()).unwrap_or(u32::MAX);
            if K32GetProcessMemoryInfo(GetCurrentProcess(), &raw mut counters, counters.cb) == 0 {
                return 0;
            }
            u64::try_from(counters.WorkingSetSize).unwrap_or(u64::MAX)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn percentile_handles_empty_and_ordered_samples() {
        assert!(percentile(&[], 95).abs() < f64::EPSILON);
        let values = [0.0, 1.0, 2.0, 3.0, 4.0];
        assert!((percentile(&values, 50) - 2.0).abs() < f64::EPSILON);
        assert!((percentile(&values, 95) - 4.0).abs() < f64::EPSILON);
    }

    #[test]
    fn process_snapshot_keeps_javascript_memory_keys() {
        let memory = process_memory_snapshot();
        for key in ["rss", "heapTotal", "heapUsed", "external", "arrayBuffers"] {
            assert!(memory.get(key).is_some());
        }
    }
}
