# Preserve constrained execution capacity

Execution class capacities are nested ranges in the shared slot pool. With
10 total slots, heavy capacity 5 and I/O capacity 2, an I/O request of weight 2
must acquire slots 0 and 1. Previously, a light job took slot 0 first, and a
heavy job took slots 0 and 1 first. Either could block I/O until it completed
despite sufficient free slots elsewhere in the pool.

The C++23 scheduler now scans each execution request's eligible range from
highest to lowest. Less-constrained jobs consequently use the slots that
constrained jobs cannot use. Watch-pool ordering, class capacities, weights,
interactive reservations, FIFO tickets, background aging, disk-pressure
exclusions, resource-vector admission and ownership verification are unchanged.
This is a placement improvement; it does not preempt running leases or promise
that an already saturated class will accept more work.

## Regression coverage

`scheduler_tests.cpp` holds light and heavy leases from both interactive and
background callers, then requires an I/O job to acquire its full weight before
the unrelated holder exits. It also verifies that another I/O job stays queued
at the class limit and can acquire the released capacity while that holder is
still active. The regression fails on the preceding implementation. Existing
cross-process contention, cancellation, timeout, stale/live owner identity,
FIFO, reserved capacity, resource-vector and pressure tests remain enabled.

## Matched Windows timing

The standalone `devbox-scheduler-placement-bench` uses new isolated state and an
80 ms unrelated holder. Two warmups precede 20 observations for each holder
class. Four matched pairs ran in ABBAABBA order using MSVC 19.51 Release builds
from base `d70df7c92e223bc7fd10fddba068fd3ee88a9577`. No production requests or
state were used. Raw samples, binary hashes and the sequence are retained in
[the benchmark evidence](benchmarks/scheduler-placement-20261002.json). Summaries
use the conventional median and nearest-rank p95. They were recomputed from
the unchanged saved timings after correcting the reporting indices; the
recorded executable hashes identify the original measurement runs.

| Case | Before median / p95 | After median / p95 | Samples per variant |
| --- | ---: | ---: | ---: |
| I/O admission with a light holder | 91.640 / 93.147 ms | 1.602 / 1.786 ms | 80 |
| I/O admission with a heavy holder | 90.820 / 92.114 ms | 1.552 / 1.904 ms | 80 |
| Uncontended acquire/release | 1.225 / 1.649 ms | 1.272 / 1.580 ms | 4,000 |

The large reduction removes waiting for the synthetic unrelated holder; it is
not an application-wide throughput claim. Uncontended median rose by 0.047 ms
while p95 fell by 0.070 ms in this run. There is no demonstrated general
uncontended speedup. Oracle and other platforms require their own deployment
smokes and hosted qualification.

Build with `DEVBOX_BUILD_BENCHMARKS=ON`, then run:

```text
devbox-scheduler-placement-bench NEW_ABSOLUTE_FIXTURE_DIRECTORY
devbox-component-bench ANOTHER_NEW_ABSOLUTE_FIXTURE_DIRECTORY --scheduler-only
```

Use the same fixture program on both versions, distinct state directories,
alternating ordering and exact executable digests when reproducing the result.
