#![allow(unsafe_code)]

//! Small Win32 process primitives used on latency-sensitive MCP paths.

use std::{
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

use serde_json::{Value, json};
use windows_sys::Win32::{
    Foundation::{CloseHandle, WAIT_TIMEOUT},
    System::Threading::{OpenProcess, PROCESS_SYNCHRONIZE, WaitForSingleObject},
};

static PROBE_COUNT: AtomicU64 = AtomicU64::new(0);
static PROBE_TOTAL_NS: AtomicU64 = AtomicU64::new(0);
static PROBE_MAX_NS: AtomicU64 = AtomicU64::new(0);

/// Return whether `pid` currently names a running process without spawning `tasklist.exe`.
#[must_use]
pub fn process_alive(pid: u32) -> bool {
    let started = Instant::now();
    let alive = if pid == 0 {
        false
    } else {
        unsafe {
            let handle = OpenProcess(PROCESS_SYNCHRONIZE, 0, pid);
            if handle.is_null() {
                return false;
            }
            let result = WaitForSingleObject(handle, 0);
            let _ = CloseHandle(handle);
            result == WAIT_TIMEOUT
        }
    };
    let nanos = u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX);
    PROBE_COUNT.fetch_add(1, Ordering::Relaxed);
    PROBE_TOTAL_NS.fetch_add(nanos, Ordering::Relaxed);
    PROBE_MAX_NS.fetch_max(nanos, Ordering::Relaxed);
    alive
}

#[must_use]
pub fn metrics_snapshot() -> Value {
    let count = PROBE_COUNT.load(Ordering::Relaxed);
    let total = PROBE_TOTAL_NS.load(Ordering::Relaxed);
    let max = PROBE_MAX_NS.load(Ordering::Relaxed);
    let average = Duration::from_nanos(total.checked_div(count).unwrap_or(0));
    let maximum = Duration::from_nanos(max);
    json!({
        "count": count,
        "averageMs": average.as_secs_f64() * 1_000.0,
        "maxMs": maximum.as_secs_f64() * 1_000.0,
        "backend": "win32-openprocess",
    })
}
