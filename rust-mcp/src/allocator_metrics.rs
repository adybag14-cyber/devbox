#![allow(unsafe_code)]

//! Low-overhead requested-byte counters for Rust's process-wide system allocator.
//!
//! These counters intentionally measure bytes requested through `GlobalAlloc`, not
//! allocator-reserved arenas or RSS. They complement OS process memory telemetry
//! without pretending the Node-compatible heap fields have Rust equivalents.

use std::{
    alloc::{GlobalAlloc, Layout, System},
    sync::atomic::{AtomicU64, Ordering},
};

use serde_json::{Value, json};

pub struct TrackingSystemAllocator;

#[global_allocator]
static GLOBAL_ALLOCATOR: TrackingSystemAllocator = TrackingSystemAllocator;

static CURRENT_REQUESTED_BYTES: AtomicU64 = AtomicU64::new(0);
static PEAK_REQUESTED_BYTES: AtomicU64 = AtomicU64::new(0);
static CUMULATIVE_ALLOCATED_BYTES: AtomicU64 = AtomicU64::new(0);
static CUMULATIVE_FREED_BYTES: AtomicU64 = AtomicU64::new(0);
static ACTIVE_ALLOCATIONS: AtomicU64 = AtomicU64::new(0);
static ALLOCATION_CALLS: AtomicU64 = AtomicU64::new(0);
static DEALLOCATION_CALLS: AtomicU64 = AtomicU64::new(0);
static REALLOCATION_CALLS: AtomicU64 = AtomicU64::new(0);

unsafe impl GlobalAlloc for TrackingSystemAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            record_new_allocation(layout.size());
        }
        pointer
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc_zeroed(layout) };
        if !pointer.is_null() {
            record_new_allocation(layout.size());
        }
        pointer
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) };
        record_deallocation(layout.size());
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        let replacement = unsafe { System.realloc(pointer, layout, new_size) };
        REALLOCATION_CALLS.fetch_add(1, Ordering::Relaxed);
        if !replacement.is_null() {
            let old_size = bytes(layout.size());
            let new_size = bytes(new_size);
            if new_size >= old_size {
                let growth = new_size - old_size;
                let current = add_saturating(&CURRENT_REQUESTED_BYTES, growth);
                add_saturating(&CUMULATIVE_ALLOCATED_BYTES, growth);
                PEAK_REQUESTED_BYTES.fetch_max(current, Ordering::Relaxed);
            } else {
                let shrink = old_size - new_size;
                sub_saturating(&CURRENT_REQUESTED_BYTES, shrink);
                add_saturating(&CUMULATIVE_FREED_BYTES, shrink);
            }
        }
        replacement
    }
}

fn record_new_allocation(size: usize) {
    let requested = bytes(size);
    let current = add_saturating(&CURRENT_REQUESTED_BYTES, requested);
    PEAK_REQUESTED_BYTES.fetch_max(current, Ordering::Relaxed);
    add_saturating(&CUMULATIVE_ALLOCATED_BYTES, requested);
    add_saturating(&ACTIVE_ALLOCATIONS, 1);
    add_saturating(&ALLOCATION_CALLS, 1);
}

fn record_deallocation(size: usize) {
    let requested = bytes(size);
    sub_saturating(&CURRENT_REQUESTED_BYTES, requested);
    add_saturating(&CUMULATIVE_FREED_BYTES, requested);
    sub_saturating(&ACTIVE_ALLOCATIONS, 1);
    add_saturating(&DEALLOCATION_CALLS, 1);
}

fn add_saturating(counter: &AtomicU64, amount: u64) -> u64 {
    let mut current = counter.load(Ordering::Relaxed);
    loop {
        let next = current.saturating_add(amount);
        match counter.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return next,
            Err(actual) => current = actual,
        }
    }
}

fn sub_saturating(counter: &AtomicU64, amount: u64) -> u64 {
    let mut current = counter.load(Ordering::Relaxed);
    loop {
        let next = current.saturating_sub(amount);
        match counter.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return next,
            Err(actual) => current = actual,
        }
    }
}

fn bytes(size: usize) -> u64 {
    u64::try_from(size).unwrap_or(u64::MAX)
}

#[must_use]
pub fn snapshot() -> Value {
    json!({
        "backend": "std::alloc::System tracked requested bytes",
        "currentRequestedBytes": CURRENT_REQUESTED_BYTES.load(Ordering::Relaxed),
        "peakRequestedBytes": PEAK_REQUESTED_BYTES.load(Ordering::Relaxed),
        "cumulativeAllocatedBytes": CUMULATIVE_ALLOCATED_BYTES.load(Ordering::Relaxed),
        "cumulativeFreedBytes": CUMULATIVE_FREED_BYTES.load(Ordering::Relaxed),
        "activeAllocations": ACTIVE_ALLOCATIONS.load(Ordering::Relaxed),
        "allocationCalls": ALLOCATION_CALLS.load(Ordering::Relaxed),
        "deallocationCalls": DEALLOCATION_CALLS.load(Ordering::Relaxed),
        "reallocationCalls": REALLOCATION_CALLS.load(Ordering::Relaxed),
    })
}

#[cfg(test)]
mod tests {
    use std::hint::black_box;

    use super::*;

    #[test]
    fn tracked_allocator_reports_real_requested_byte_activity() {
        let before_allocated = CUMULATIVE_ALLOCATED_BYTES.load(Ordering::Relaxed);
        let before_calls = ALLOCATION_CALLS.load(Ordering::Relaxed);
        let data = black_box(vec![0_u8; 4096]);
        let during_allocated = CUMULATIVE_ALLOCATED_BYTES.load(Ordering::Relaxed);
        let during_calls = ALLOCATION_CALLS.load(Ordering::Relaxed);
        assert!(during_allocated >= before_allocated.saturating_add(4096));
        assert!(during_calls > before_calls);
        drop(data);
        let snapshot = snapshot();
        assert_eq!(
            snapshot["backend"],
            "std::alloc::System tracked requested bytes"
        );
        assert!(snapshot["peakRequestedBytes"].as_u64().unwrap_or(0) > 0);
        assert!(snapshot["cumulativeFreedBytes"].as_u64().unwrap_or(0) > 0);
    }
}
