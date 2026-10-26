# Bounded telemetry history — 26 October 2026

The event-loop monitor retains up to 15,000 delay samples and five minutes of
one-second drift samples. Its previous `std::deque` storage uses many small
allocations on MSVC. The C++23 implementation now allocates two contiguous
buffers once, retaining the same chronological samples and inclusive five-minute
expiry boundary. The delay limit remains 15,000. Drift can be sampled at most
once per second, so 301 entries cover its inclusive window.

The buffers reserve about 239 KiB at construction instead of growing during
warmup. This trades a small initial reservation for a lower, fixed warm memory
footprint. Timer cadence, snapshot fields, percentile calculations and locking
are unchanged. Snapshot copies still allocate outside the history storage.

## Reproduce the component comparison

Configure with `DEVBOX_BUILD_BENCHMARKS=ON`, build
`devbox-performance-window-bench`, then run that executable. It compares the
production window class with the previous deque algorithm using Devbox's native
allocation counters. Output is JSON Lines. The benchmark runs three ABBA cycles,
giving six observations per variant for each mode, and rejects mismatched
sample/percentile checksums.

Each observation processes 180,000 synthetic 20 ms timer ticks, equivalent to an
hour of history without waiting an hour. The capture mode also performs the
existing 10/60/300-second percentile calculations every 250 ticks. This is an
accelerated component measurement, not a live server or whole-agent benchmark.

Measured locally with Windows x64, MSVC 19.51.36260.0, C++23, Release optimization,
interprocedural optimization disabled, and native allocation tracking enabled:

| Measurement | Previous deque | Contiguous buffers |
|---|---:|---:|
| Retained history bytes after the trace | 405,614 | 244,894 |
| Allocation calls, ingest and final checksum snapshot | 16,919 | 4 |
| Allocation calls, including periodic captures | 63,496 | 46,581 |
| Ingest median across 180,000 ticks | 1.11490 ms | 0.55475 ms |
| Ingest plus capture median | 461.19710 ms | 417.28245 ms |
| Ingest plus capture observed range | 413.7949–524.8295 ms | 411.7905–496.4628 ms |

Retained storage fell by 160,720 bytes (about 157 KiB). The capture timing ranges
overlap; these measurements do not establish a whole-server CPU or response-time
improvement. Allocation layout differs between standard libraries, so the memory
numbers above describe this MSVC build rather than every deployment platform.

## Correctness checks

The `performance-window` test compares retained samples with an independent
chronological deque reference across ordinary cadence, equal timestamps after
timer delays, repeated wraparound and long suspensions. It explicitly checks zero
capacity, one-entry capacity, clearing a reused snapshot, exact expiry boundaries
and the full delay/drift capacities. The existing `telemetry` integration test
still injects a real executor stall and verifies the published event-loop and
process metrics. Local Windows engine/telemetry/window tests and a GCC 12
AddressSanitizer/UndefinedBehaviorSanitizer run passed.

This change is separate from `bootstrap-v0.9.10`. It must pass its own hosted
qualification before merge or any later release; these results do not imply that
it has been deployed.
