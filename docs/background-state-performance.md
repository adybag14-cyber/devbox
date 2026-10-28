# Native background status allocation cost

Background worker updates now keep their counters, timestamps, running flag and
error text in typed C++23 state. JSON is constructed only when a caller requests
a snapshot. The established field names, field order, null handling, sorted worker
names, restart counters and failure history are preserved. Snapshots still own
their values, and all state access uses the same mutex.

Snapshot construction reserves the known object sizes. Because worker names come
from a map and the fourteen output fields are fixed and unique, it appends them
directly to the ordered JSON container. This avoids repeated duplicate-key searches
while constructing the result; it does not bypass any check on caller input.

## Controlled component measurement

The `devbox-background-bench` target exercises attempt/success updates, failure
recovery and snapshots with 1, 14 and 128 workers. It reports wall time, allocation
calls, allocated bytes and retained allocation change separately. No production
state, filesystem workers, durable runners or browser sessions are used.

The 28 October 2026 Windows MSVC Release comparison used the implementation at
`1f2f190a13cef0f93f10cda8818e6434c663a269` as its baseline, the same benchmark logic
and build configuration, and three processes per variant in alternating order.
Each process reported five trials per phase. The following are medians of those
15 trials; times are microseconds per operation.

| Workers | Operation | Before, us | After, us | Allocations before | Allocations after |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Attempt and success | 0.23 | 0.05 | 3 | 0 |
| 1 | Snapshot | 1.47 | 0.99 | 32 | 19 |
| 14 | Attempt and success | 0.24 | 0.06 | 3 | 0 |
| 14 | Failure and recovery | 0.49 | 0.09 | 7 | 1 |
| 14 | Snapshot | 34.47 | 7.05 | 693 | 131 |
| 128 | Snapshot | 341.45 | 103.38 | 5608 | 1156 |

Every measured phase ended with zero retained allocation growth. The benchmark
uses a mixture of periodic and event-driven workers. Its results are component
measurements on one machine; they do not establish a whole-server latency or CPU
improvement, nor do they prove that all server paths are free of memory leaks.

The `background-state` test checks exact initial wire output, field order, event
ages, failure/recovery history, restart counters, detached snapshot ownership and
concurrent updates. Existing telemetry and lifecycle tests exercise the callers.

## Full-server fixture requirements

`devbox-server-lifetime-bench` now verifies that the isolated durable coordinator
actually responds before starting its measurements. At the end it also checks
the maintenance and quota workers for reported errors. A loopback `/readyz` pass
alone is insufficient to certify this benchmark's backend.

Run either `devbox-idle-components-bench` or `devbox-server-lifetime-bench` with a
new absolute fixture directory. On Windows a containing job object must permit
the fixture's independent coordinator to break away, or the preflight deliberately
fails. Normal completion stops that coordinator. A supervising test harness can
also invoke the same benchmark executable with `--stop-fixture-coordinator` and
the fixture's state directory to request authenticated cleanup and confirm the
stored process birth identity is no longer alive. These helpers are optional
benchmark targets; they add no installed runtime dependency.
