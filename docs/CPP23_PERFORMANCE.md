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
