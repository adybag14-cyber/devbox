# Exact percentile selection with reusable storage

The C++23 performance monitor computes the same 10/60/300-second statistics using
partial selection instead of sorting every delay. It finds the chronological
window boundary with binary search, reuses one scratch vector across the three
windows, and preserves selected ranks as it partitions the remaining values.
For at most eight distinct values it uses a bounded, exact frequency table
instead. If a ninth distinct value appears, every sample remains in the scratch
buffer and the general selection path handles the entire window. This avoids
the measured repeated-value slowdown of applying partial selection alone.
The rank remains `round((sample_count - 1) * percentile / 100)` for p50, p95 and
p99. The integer calculation avoids an intermediate size multiplication overflow.
Empty-window values, maximum delay, maximum drift, inclusive expiry, sample count,
capture locking and the published JSON fields are unchanged.

The scratch buffer belongs to one capture. It is not shared between threads or
retained after the capture, and no additional dependency is introduced. Samples
remain in chronological order; only the private vector of finite delay values is
partitioned. This builds on the independently qualified contiguous history
storage described in [the history measurements](performance-telemetry-window.md).

## Correctness and measurement

The `performance-statistics` test compares every output field against a separate
full-sort reference. It covers every cardinality from zero through 350 with
constant, ascending, descending, tied/spiky and pseudorandom values; repeated
timestamps; inclusive cutoffs; fully expired histories; and a full 15,000-sample
history. Eight/nine-value traces and a late table overflow test both calculation
paths. It verifies that source samples are unchanged and reserved scratch
storage is reused. Windows Release and Linux GCC AddressSanitizer /
UndefinedBehaviorSanitizer runs passed, along with the Windows engine and
telemetry integration tests.

Configure `DEVBOX_BUILD_BENCHMARKS=ON`, build
`devbox-performance-statistics-bench`, and run it to reproduce the component
comparison. It uses the same production history buffers in both variants,
processes 180,000 simulated 20 ms ticks, and captures the three windows 360 times,
matching the monitor's steady ten-second persistence interval.
Three ABBA cycles give six observations per variant and distribution. Every
sample count, percentile, maximum and drift value contributes to a checksum;
the executable rejects a mismatch.

Measured on Windows x64, MSVC 19.51.36260.0, Release, C++23, with interprocedural
optimization disabled and native allocation tracking enabled:

| One accelerated hour of captures | Full sort | Selection and shared scratch |
|---|---:|---:|
| Allocation calls, either trace | 23,265 | 1,082 |
| Cumulative requested allocation bytes, either trace | 272,447,309 | 125,965,963 |
| Quantized-delay trace median | 168.762 ms | 71.340 ms |
| Quantized-delay observed range | 166.688–182.649 ms | 71.079–72.834 ms |
| Ties/spikes trace median | 30.127 ms | 24.279 ms |
| Ties/spikes observed range | 29.705–30.691 ms | 24.073–25.031 ms |

Allocation traffic and elapsed time fell in both of these traces. This does not
establish that this algorithm is faster for every distribution. Cumulative requested
bytes measure allocation traffic, not retained or resident memory. These are
accelerated component measurements and do not establish faster website loading
or a whole-agent speedup. Publishing or merging this code does not establish
that it is running in production.
