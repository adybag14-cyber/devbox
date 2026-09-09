# C++23 performance acceptance

Production was explicitly restored to the frozen Rust build before this work.
The C++23 work proceeds in an isolated checkout; a language-standard change
alone is not evidence of a performance improvement.

The reference is the previous production Rust executable built from
`cd8803c81ad3a14ec3b3fa2afa0d08256975b1e9`, SHA-256
`6b33147a1032368a293557705811047cd4be01b080000de0de7fc2f4dcad0114`.
Its code, binary, configuration limits, and benchmark workloads remain fixed.

## Required evidence

- All 45 tool contracts, authorization behavior, persisted state, cancellation,
  process ownership, output integrity, and applicable native/platform tests pass.
- Repeated paired runs compare the same workloads and payloads, alternating the
  execution order, with warm-up excluded and no production server in the test.
- Compare startup, sequential and concurrent RPC latency/throughput, file reads,
  atomic writes, native program execution, durable jobs, health responsiveness
  with pending waits, working-set/private memory, executable size, and CPU cost.
- Compare median and tail latency and record per-run variation. A difference
  smaller than measurement resolution must not be presented as a strict win.
- Both normal connection behavior and the forced-new-connection diagnostic are
  retained; the matched 100 requests/second follow-up is retained as well.
- Request errors and telemetry loss are explicit gates. Faster execution that
  drops logging records is not an acceptable performance win. Correctness metrics
  whose optimum is zero must remain zero.
- Final claims use a clean committed C++23 build with its executable hash and
  source identity recorded. Intermediate dirty builds are development evidence.

## Initial optimization targets

The C++20 comparison exposed one-request HTTP connections, a slow telemetry
writer under bursts, additional file/process latency, and slightly higher idle
private memory. The first changes target those measured costs while retaining
disconnect cancellation, bounded queues, durability, and complete output.

Progress is measured against the original suite, not by reducing payloads,
changing the Rust build, disabling instrumentation/logging, or weakening checks.

## Implementation and reproducible diagnosis

The runtime reuses HTTP connections with a separate bounded read-ahead operation
that preserves pipelined input and detects disconnects. Request authentication,
headers, cancellation and registry state are rebuilt for every request. Completed
tool results select ordinary JSON when its accepted quality is at least the SSE
quality; clients that prefer SSE receive one complete SSE response. Longer
operations retain periodic heartbeats and scoped cancellation. Both response
representations are part of the
[Streamable HTTP contract](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).

Large JSON RPC responses negotiate gzip or zlib-wrapped deflate only when the
client accepts that content coding. Explicit coding exclusions and higher identity
preferences are retained, and gateway `Vary` fields are combined. Compression
uses a fresh dictionary for each response. For responses through one MiB,
libdeflate's fastest compressor writes directly into the C++23 string overwrite
buffer. Larger responses use incremental zlib with a 64 KiB output buffer and
reduced working memory. An 8 KiB prefix must compress by at least half before
the remaining input is processed; low-compressibility responses keep their
original bytes. Both paths check cancellation, and incremental input chunks
yield when their time slice is used. Compression is limited to responses from
128 KiB through 16 MiB;
small responses, SSE streams and OAuth endpoints retain their existing paths.
This is a transport optimization with decoded payload equality tests, including
varied one MiB results that span multiple compressed blocks. It follows HTTP
[content coding negotiation](https://www.rfc-editor.org/rfc/rfc9110.html#section-12.5.3)
and the [zlib stream API](https://www.zlib.net/manual.html). The whole-buffer path
uses the [libdeflate API](https://github.com/ebiggers/libdeflate), pinned by the
same vcpkg baseline as the other native dependencies.
Each HTTP write is bounded to 64 KiB. Printable JSON strings reserve space for
their closing delimiters, avoiding a second large allocation and copy.

Capability metadata is immutable per engine and its schema digest is computed
once. Result construction moves large payloads, telemetry borrows the fields it
summarizes, and compact JSON output validates printable ASCII before copying it.
Other strings and numeric encodings retain the reference serializer. Differential
tests compare bytes, invalid UTF-8 policies, escapes and nested values.
Only the selected runtime profile is materialized from the frozen schema input
during startup; both profiles continue to pass the contract checks.

Cancellable timers subscribe to direct and ancestor cancellation rather than
waking every 50 ms. Timer arming, cancellation and destruction share a strand;
subscription teardown waits for a callback already in flight and prevents later
callbacks from accessing a destroyed executor. Tests cover concurrent direct and
ancestor cancellation, cancelled tokens, dropped executors and normal timer
deadlines without polling. Synchronous parent-token waits use notifications too.
The Windows main loop waits on its shutdown flag with `WaitOnAddress`; `SIGINT`,
`SIGTERM` and console events wake it directly. Integration tests run the real
runtime, wait for readiness, raise each signal within that same test process, and
require successful shutdown with its workers joined.

Windows thread-count telemetry queries the documented `SystemProcessInformation`
records using dynamically resolved `NtQuerySystemInformation`, with bounded
buffer growth and record validation. The existing Toolhelp enumeration remains
the fallback. Counting the current process's threads no longer requires visiting
every system thread in user space. A test compares the result with independent
Toolhelp enumeration while three known workers remain alive. The existing
60-second cache and reported metric remain intact.

Worker pools start threads as demand requires while retaining their configured
concurrency and queue limits. Logging batches at most 64 accepted events with a
2 ms coalescing window; every batch retains checked stream flushing, rotation and
failure accounting. This does not add a power-loss durability guarantee to usage
logs. Atomic files and durable state retain their explicit operating-system
flushes and verification.

Existing directories use a status check before recursive creation. Atomic writes
retain private lock validation, target version checks, staging verification,
permission handling and atomic replacement. Immediate scheduler admission uses
the same cross-process claim and cannot bypass a real queued ticket or aged
background work. Windows process completion rechecks its pipes immediately after
exit, and absent input uses a noninteractive EOF handle. Job status polling starts
with a shorter interval and backs off for longer waits without extending caller
deadlines.
Scheduler inspection retries transient Windows sharing/delete-pending failures
for at most 100 ms. Persistent unreadability remains an error; it never licenses
reclaiming a live slot. Tests preserve ownership across transient and persistent
sharing conflicts and repeated contention between child processes.

MSVC Release builds enable whole-program optimization unless explicitly disabled
through `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`. C++23 uses the explicit
`/std:c++23preview` language mode on supported MSVC versions, rather than enabling
later experimental language revisions.

For component diagnosis, configure `DEVBOX_BUILD_BENCHMARKS=ON` and build
`devbox-component-bench`. Run it with a new absolute fixture directory whose
parent already exists. It reports JSON timing records for file, serialization,
logging, scheduler, process and engine operations, validates results and retains
its fixtures for inspection. These component timings do not replace the paired
end-to-end comparison or its correctness gates.
