# Native readiness allocation reduction

The readiness endpoint needs a fresh scheduler snapshot and a running, successful
execution-store probe. Previously it copied the complete cached scheduler payload,
constructed diagnostic JSON for every background worker, and copied the store-health
JSON to inspect a few fields. The C++23 implementation now reads those cached fields
under their existing mutex and checks the selected worker's typed state directly.
The monitor and worker locks remain separate. The 5-second scheduler and 150-second
store/worker freshness limits, complete-tool-count requirement, failure handling,
and diagnostic snapshot format are unchanged.

Timestamp age calculation also reads the existing string by reference and samples
the wall clock once. Missing or malformed timestamps remain unavailable; future
timestamps still clamp to zero age and pre-epoch timestamps retain their established
handling. No runtime dependency or MCP action is added.

## Measurement

On 28 October 2026, three fresh process trials per variant ran on the same Windows
workstation with alternating order. Both used MSVC 19.51 Release builds with IPO and
the same dependency SDK. Each trial required a responding native state coordinator,
waited 20 seconds, issued 200 loopback readiness requests followed by 200 metadata
requests, waited another 20 seconds, and verified native coordinator shutdown.
Every trial used separate private state. No production state or service was changed.

The baseline source tree was `5c43d41c23295a64d16a68e87db58f551c3ea149`, identical to
release source `801c24f7cccc007cf4dc914e1ebb11ad77181475`. The candidate code commit was
`196f9907a040d45acc996e57b955f6c12001c5d7`. Each executable's embedded source identity
and SHA-256 were checked. An earlier binary with pre-commit identity was rejected
and excluded before these six qualified trials.

Medians across the three trials per variant:

| Phase / metric | Baseline | Candidate | Change |
| --- | ---: | ---: | ---: |
| 200 readiness requests: allocation calls | 158,477 | 101,047 | 36.2% fewer |
| 200 readiness requests: requested bytes | 15,325,227 | 9,519,847 | 37.9% fewer |
| 200 readiness requests: elapsed time | 249.18 ms | 237.57 ms | 4.7% lower in this sample |
| 200 metadata requests: allocation calls | 254,943 | 254,037 | 0.4% fewer |
| 200 metadata requests: requested bytes | 25,677,703 | 25,692,123 | 0.1% more |
| 200 metadata requests: elapsed time | 256.85 ms | 242.37 ms | 5.6% lower in this sample |

The allocation saving is the supported result. The unchanged metadata control also
became faster by a similar amount, so this experiment does not establish a causal
elapsed-time improvement. The workstation was shared, the sample is small, and the
counters include the benchmark's native HTTP client and periodic background work.
These are not end-to-end ChatGPT/browser timings or a long-run leak-freedom result.
All individual readiness/control measurements are in
[the measurement record](benchmarks/readiness-allocation-20261028.json).

## Validation and reproduction

The `engine`, `background-state`, `telemetry`, and `core` suites passed on Windows
and Linux/GCC 12.3. Regression coverage includes missing, stopped, failed, expired,
and recovered workers; bounded string views; concurrent failures; timestamp edge
cases; and readiness after real scheduler/storage probes. A thousand typed worker
health queries allocate no C++ diagnostic objects.

Configure with `DEVBOX_BUILD_TESTS=ON` and `DEVBOX_BUILD_BENCHMARKS=ON`, then build
`devbox-server-lifetime-bench` and the relevant test targets. Run the benchmark with
a **new absolute fixture directory** as its sole argument. Repeat with unchanged
compiler/SDK settings at the baseline and candidate revisions, alternating variants.
Use the phase JSON records for comparison and require the final healthy resource
snapshot and successful native coordinator cleanup. A wrapper that prohibits the
coordinator's separate process lifetime makes the fixture invalid; do not accept a
degraded run or relax its checks to obtain a timing number.
