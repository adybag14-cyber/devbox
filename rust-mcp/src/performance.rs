use std::{
    collections::VecDeque,
    path::PathBuf,
    sync::{Arc, Mutex, MutexGuard, RwLock},
    time::{Duration, Instant},
};

use chrono::{SecondsFormat, Utc};
use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use crate::background::BackgroundTaskRegistry;

const SAMPLE_INTERVAL: Duration = Duration::from_millis(20);
const DRIFT_INTERVAL: Duration = Duration::from_secs(1);
const PERSIST_INTERVAL: Duration = Duration::from_secs(10);
const PERSIST_STALE_AFTER: Duration = Duration::from_secs(30);
const MAX_SAMPLES: usize = 15_000;
const SHORT_WINDOW: Duration = Duration::from_secs(10);
const ONE_MINUTE: Duration = Duration::from_secs(60);
const FIVE_MINUTES: Duration = Duration::from_secs(300);
const HISTORY_MAX_BYTES: u64 = 4 * 1024 * 1024;
const HISTORY_ROTATIONS: usize = 3;

#[derive(Debug, Clone, Copy)]
struct TimedSample {
    at: Instant,
    value_ms: f64,
}

#[derive(Debug, Clone, Default)]
struct PerformanceWindow {
    delays: VecDeque<TimedSample>,
    drifts: VecDeque<TimedSample>,
}

#[derive(Debug)]
struct PerformanceInner {
    state: Arc<Mutex<PerformanceWindow>>,
    cached: Arc<RwLock<Value>>,
    cached_at: Arc<RwLock<Instant>>,
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
    pub fn new(state_path: PathBuf, background: &BackgroundTaskRegistry) -> Self {
        let state = Arc::new(Mutex::new(PerformanceWindow::default()));
        let started_at = Instant::now();
        let cached = Arc::new(RwLock::new(snapshot_value(&state, started_at)));
        let cached_at = Arc::new(RwLock::new(Instant::now()));
        let inner = Arc::new(PerformanceInner {
            state,
            cached,
            cached_at,
            started_at,
            cancellation: CancellationToken::new(),
        });
        spawn_sampler(&inner, background);
        spawn_persistence(&inner, state_path, background);
        Self { inner }
    }

    #[must_use]
    pub fn snapshot(&self) -> Value {
        let snapshot = self
            .inner
            .cached
            .read()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone();
        let cached_at = *self
            .inner
            .cached_at
            .read()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        decorate_cache_freshness(snapshot, cached_at.elapsed())
    }
}

fn spawn_sampler(inner: &PerformanceInner, background: &BackgroundTaskRegistry) {
    let state = inner.state.clone();
    background.spawn_supervised(
        "performance-sampler",
        inner.cancellation.child_token(),
        move |cancellation, heartbeat| {
            let state = state.clone();
            async move {
                let mut expected_sample = tokio::time::Instant::now() + SAMPLE_INTERVAL;
                let mut expected_drift = tokio::time::Instant::now() + DRIFT_INTERVAL;
                loop {
                    tokio::select! {
                        () = cancellation.cancelled() => return Ok(()),
                        () = tokio::time::sleep_until(expected_sample) => {
                            let now = tokio::time::Instant::now();
                            let delay_ms = now.saturating_duration_since(expected_sample).as_secs_f64() * 1_000.0;
                            let mut window = lock_window(&state);
                            window.delays.push_back(TimedSample { at: now.into_std(), value_ms: delay_ms });
                            while window.delays.len() > MAX_SAMPLES { window.delays.pop_front(); }
                            if now >= expected_drift {
                                let drift_ms = now.saturating_duration_since(expected_drift).as_secs_f64() * 1_000.0;
                                window.drifts.push_back(TimedSample { at: now.into_std(), value_ms: drift_ms });
                                expected_drift = now + DRIFT_INTERVAL;
                                heartbeat.tick();
                            }
                            if let Some(cutoff) = Instant::now().checked_sub(FIVE_MINUTES) {
                                while window.delays.front().is_some_and(|sample| sample.at < cutoff) { window.delays.pop_front(); }
                                while window.drifts.front().is_some_and(|sample| sample.at < cutoff) { window.drifts.pop_front(); }
                            }
                            drop(window);
                            expected_sample += SAMPLE_INTERVAL;
                            if now.saturating_duration_since(expected_sample) > Duration::from_secs(1) { expected_sample = now + SAMPLE_INTERVAL; }
                        }
                    }
                }
            }
        },
    );
}

fn spawn_persistence(
    inner: &PerformanceInner,
    state_path: PathBuf,
    background: &BackgroundTaskRegistry,
) {
    let state = inner.state.clone();
    let cached = inner.cached.clone();
    let cached_at = inner.cached_at.clone();
    let started_at = inner.started_at;
    let history_path = state_path.with_file_name("mcp-performance-history.jsonl");
    background.spawn_supervised(
        "performance-persistence",
        inner.cancellation.child_token(),
        move |cancellation, heartbeat| {
            let state = state.clone();
            let cached = cached.clone();
            let cached_at = cached_at.clone();
            let state_path = state_path.clone();
            let history_path = history_path.clone();
            async move {
                let mut interval = tokio::time::interval(PERSIST_INTERVAL);
                loop {
                    tokio::select! {
                        () = cancellation.cancelled() => return Ok(()),
                        _ = interval.tick() => {
                            let snapshot = snapshot_value(&state, started_at);
                            *cached.write().unwrap_or_else(std::sync::PoisonError::into_inner) = snapshot.clone();
                            *cached_at.write().unwrap_or_else(std::sync::PoisonError::into_inner) = Instant::now();
                            if let Err(error) = persist_snapshot(&state_path, &snapshot).await {
                                tracing::debug!(%error, path = %state_path.display(), "failed to persist Rust MCP performance snapshot");
                            }
                            if let Err(error) = append_history(&history_path, &snapshot).await {
                                tracing::debug!(%error, path = %history_path.display(), "failed to append Rust MCP performance history");
                            }
                            heartbeat.tick();
                        }
                    }
                }
            }
        },
    );
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

async fn append_history(path: &std::path::Path, snapshot: &Value) -> std::io::Result<()> {
    use tokio::io::AsyncWriteExt as _;
    if let Some(parent) = path.parent() {
        tokio::fs::create_dir_all(parent).await?;
    }
    if tokio::fs::metadata(path)
        .await
        .map(|m| m.len())
        .unwrap_or(0)
        >= HISTORY_MAX_BYTES
    {
        let oldest = history_rotation_path(path, HISTORY_ROTATIONS);
        tokio::fs::remove_file(&oldest).await.ok();
        for index in (1..HISTORY_ROTATIONS).rev() {
            let from = history_rotation_path(path, index);
            let to = history_rotation_path(path, index + 1);
            let _ = tokio::fs::rename(&from, &to).await;
        }
        let _ = tokio::fs::rename(path, history_rotation_path(path, 1)).await;
    }
    let compact = json!({
        "sampledAtUtc": snapshot.pointer("/eventLoop/sampledAtUtc"),
        "pid": snapshot.pointer("/process/pid"),
        "p95Ms": snapshot.pointer("/eventLoop/p95Ms"),
        "p99Ms": snapshot.pointer("/eventLoop/p99Ms"),
        "maxMs": snapshot.pointer("/eventLoop/maxMs"),
        "timerDriftMaxMs": snapshot.pointer("/eventLoop/timerDriftMaxMs"),
        "rss": snapshot.pointer("/process/memory/rss"),
        "private": snapshot.pointer("/process/memory/private"),
        "allocatorCurrent": snapshot.pointer("/process/memory/allocator/currentRequestedBytes"),
        "cpuTotalMs": snapshot.pointer("/process/cpuTotalMs"),
    });
    let mut bytes = serde_json::to_vec(&compact).map_err(std::io::Error::other)?;
    bytes.push(b'\n');
    let mut file = tokio::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .await?;
    file.write_all(&bytes).await
}

fn history_rotation_path(path: &std::path::Path, index: usize) -> PathBuf {
    PathBuf::from(format!("{}.{index}", path.display()))
}

fn snapshot_value(state: &Arc<Mutex<PerformanceWindow>>, started_at: Instant) -> Value {
    let now = Instant::now();
    let window = lock_window(state).clone();
    let short = window_snapshot(&window, now, SHORT_WINDOW);
    let one_minute = window_snapshot(&window, now, ONE_MINUTE);
    let five_minute = window_snapshot(&window, now, FIVE_MINUTES);
    let platform = process_platform_snapshot();
    let cpu_total_ms = process_cpu_total_ms(&platform);
    json!({
        "eventLoop": {
            "sampledAtUtc": Utc::now().to_rfc3339_opts(SecondsFormat::Millis, true),
            "p50Ms": short["p50Ms"],
            "p95Ms": short["p95Ms"],
            "p99Ms": short["p99Ms"],
            "maxMs": short["maxMs"],
            "timerDriftMaxMs": short["timerDriftMaxMs"],
            "sampleCount": short["sampleCount"],
            "oneMinute": one_minute,
            "fiveMinute": five_minute,
        },
        "process": {
            "pid": std::process::id(),
            "uptimeSeconds": started_at.elapsed().as_secs_f64(),
            "memory": process_memory_snapshot(),
            "cpuTotalMs": cpu_total_ms,
            "platform": platform,
        },
    })
}

fn decorate_cache_freshness(mut snapshot: Value, age: Duration) -> Value {
    if let Some(object) = snapshot.as_object_mut() {
        object.insert(
            "cachedAgeMs".to_owned(),
            json!(u64::try_from(age.as_millis()).unwrap_or(u64::MAX)),
        );
        object.insert("stale".to_owned(), json!(age > PERSIST_STALE_AFTER));
    }
    snapshot
}

fn process_cpu_total_ms(platform: &Value) -> Option<u64> {
    #[cfg(windows)]
    {
        platform.get("cpuTotalMs").and_then(Value::as_u64)
    }
    #[cfg(unix)]
    {
        use nix::sys::{
            resource::{UsageWho, getrusage},
            time::TimeValLike as _,
        };
        let _ = platform;
        let usage = getrusage(UsageWho::RUSAGE_SELF).ok()?;
        let micros = usage
            .user_time()
            .num_microseconds()
            .saturating_add(usage.system_time().num_microseconds());
        u64::try_from(micros.saturating_div(1_000)).ok()
    }
    #[cfg(not(any(windows, unix)))]
    {
        let _ = platform;
        None
    }
}

fn window_snapshot(window: &PerformanceWindow, now: Instant, duration: Duration) -> Value {
    let cutoff = now.checked_sub(duration);
    let mut delays = window
        .delays
        .iter()
        .filter(|sample| cutoff.is_none_or(|cutoff| sample.at >= cutoff))
        .map(|sample| sample.value_ms)
        .collect::<Vec<_>>();
    delays.sort_by(f64::total_cmp);
    let timer_drift_max_ms = window
        .drifts
        .iter()
        .filter(|sample| cutoff.is_none_or(|cutoff| sample.at >= cutoff))
        .map(|sample| sample.value_ms)
        .fold(0.0_f64, f64::max);
    json!({
        "windowSeconds": duration.as_secs(),
        "sampleCount": delays.len(),
        "p50Ms": percentile(&delays, 50),
        "p95Ms": percentile(&delays, 95),
        "p99Ms": percentile(&delays, 99),
        "maxMs": delays.last().copied().unwrap_or(0.0),
        "timerDriftMaxMs": timer_drift_max_ms,
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
        "private": private_memory_bytes(),
        "allocator": crate::allocator_metrics::snapshot(),
        "heapTotal": 0,
        "heapUsed": 0,
        "external": 0,
        "arrayBuffers": 0,
    })
}

#[cfg(windows)]
fn private_memory_bytes() -> u64 {
    windows_memory::private_memory_bytes()
}

#[cfg(not(windows))]
const fn private_memory_bytes() -> u64 {
    0
}

#[cfg(windows)]
fn process_platform_snapshot() -> Value {
    windows_memory::process_platform_snapshot()
}

#[cfg(not(windows))]
fn process_platform_snapshot() -> Value {
    Value::Null
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

    use serde_json::{Value, json};
    use std::mem::{size_of, zeroed};
    use windows_sys::Win32::System::{
        ProcessStatus::{K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS},
        Threading::GetCurrentProcess,
    };

    fn counters() -> Option<PROCESS_MEMORY_COUNTERS> {
        unsafe {
            let mut counters: PROCESS_MEMORY_COUNTERS = zeroed();
            counters.cb = u32::try_from(size_of::<PROCESS_MEMORY_COUNTERS>()).unwrap_or(u32::MAX);
            if K32GetProcessMemoryInfo(GetCurrentProcess(), &raw mut counters, counters.cb) == 0 {
                return None;
            }
            Some(counters)
        }
    }

    pub fn resident_memory_bytes() -> u64 {
        counters()
            .and_then(|value| u64::try_from(value.WorkingSetSize).ok())
            .unwrap_or(0)
    }

    pub fn private_memory_bytes() -> u64 {
        counters()
            .and_then(|value| u64::try_from(value.PagefileUsage).ok())
            .unwrap_or(0)
    }

    pub fn process_platform_snapshot() -> Value {
        use windows_sys::Win32::{
            Foundation::{CloseHandle, FILETIME, INVALID_HANDLE_VALUE},
            System::{
                Diagnostics::ToolHelp::{
                    CreateToolhelp32Snapshot, TH32CS_SNAPTHREAD, THREADENTRY32, Thread32First,
                    Thread32Next,
                },
                Threading::{GetProcessHandleCount, GetProcessTimes},
            },
        };
        unsafe {
            let process = GetCurrentProcess();
            let mut handles = 0_u32;
            let _ = GetProcessHandleCount(process, &raw mut handles);
            let mut created: FILETIME = zeroed();
            let mut exited: FILETIME = zeroed();
            let mut kernel: FILETIME = zeroed();
            let mut user: FILETIME = zeroed();
            let cpu_total_ms = if GetProcessTimes(
                process,
                &raw mut created,
                &raw mut exited,
                &raw mut kernel,
                &raw mut user,
            ) != 0
            {
                let ticks = filetime_ticks(kernel).saturating_add(filetime_ticks(user));
                ticks / 10_000
            } else {
                0
            };
            let pid = std::process::id();
            let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            let mut threads = 0_u32;
            if snapshot != INVALID_HANDLE_VALUE {
                let mut entry: THREADENTRY32 = zeroed();
                entry.dwSize = u32::try_from(size_of::<THREADENTRY32>()).unwrap_or(u32::MAX);
                if Thread32First(snapshot, &raw mut entry) != 0 {
                    loop {
                        if entry.th32OwnerProcessID == pid {
                            threads = threads.saturating_add(1);
                        }
                        if Thread32Next(snapshot, &raw mut entry) == 0 {
                            break;
                        }
                    }
                }
                let _ = CloseHandle(snapshot);
            }
            json!({ "cpuTotalMs": cpu_total_ms, "handles": handles, "threads": threads })
        }
    }

    const fn filetime_ticks(value: windows_sys::Win32::Foundation::FILETIME) -> u64 {
        (value.dwHighDateTime as u64) << 32 | value.dwLowDateTime as u64
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
    fn cached_snapshot_exposes_staleness() {
        let fresh = decorate_cache_freshness(json!({"eventLoop": {}}), Duration::from_secs(1));
        assert_eq!(fresh["stale"], false);
        let stale = decorate_cache_freshness(json!({"eventLoop": {}}), Duration::from_secs(31));
        assert_eq!(stale["stale"], true);
        assert_eq!(stale["cachedAgeMs"], 31_000);
    }

    #[test]
    fn process_snapshot_keeps_javascript_memory_keys() {
        let memory = process_memory_snapshot();
        for key in ["rss", "heapTotal", "heapUsed", "external", "arrayBuffers"] {
            assert!(memory.get(key).is_some());
        }
        let allocator = &memory["allocator"];
        assert_eq!(
            allocator["backend"],
            "std::alloc::System tracked requested bytes"
        );
        assert!(allocator["peakRequestedBytes"].as_u64().unwrap_or(0) > 0);
        assert!(allocator["allocationCalls"].as_u64().unwrap_or(0) > 0);
    }
}
