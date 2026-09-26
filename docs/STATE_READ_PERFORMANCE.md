# Bounded SQLite read-program reuse

This performance follow-on is deliberately separate from PR77's controller-safety integration. It changes repeated SELECT preparation, not the durable state model or authorization rules.

## Optimization and limits

The native SQLite store previously prepared and finalized the same SELECT SQL on every get, count, page or event query. The candidate retains at most fifteen compiled SELECT programs per connection: get/count/events, eight combinations of optional list filters, and four bounded-count filter combinations. SQL shape determines a fixed slot; caller values remain bound parameters.

Each slot is borrowed only while the existing store mutex is held. Returning a slot resets the statement, clears every binding, releases its read transaction and resets per-execution VM-step accounting. An errored or oversized statement is finalized rather than reused. Connection destruction finalizes retained programs before closing SQLite. Retained statement memory is capped at 64 KiB per slot according to SQLite's statement-memory accounting (at most 960 KiB across the fifteen slots); this is not a bound on the connection's entire page cache or transient query/result memory.

No rows, JSON results, grants, principal decisions, writer generations or read snapshots are cached. Existing committed-state reads remain fresh. Write transactions, full durability, writer fencing, operation identities, result byte budgets, indexes and public tool schemas are unchanged. `StateStoreOptions::reuse_read_statements` is a native qualification switch, not an environment variable or model-facing setting.

The destructor's reset/clear distinction is deliberate: `sqlite3_reset` does not clear parameters. References: SQLite [reset](https://sqlite.org/c3ref/reset.html), [clear bindings](https://sqlite.org/c3ref/clear_bindings.html), [statement counters](https://sqlite.org/c3ref/c_stmtstatus_counter.html), and [prepared-statement lifecycle](https://sqlite.org/c3ref/stmt.html).

## Correctness coverage

`state-read-reuse` runs the same checks with reuse disabled and enabled. It covers every fixed SQL shape, principal/group/status rebinding, missing rows, event cursors, early page returns, four-MiB result limits, bounded distinct-status counts, VM counters, invalid arguments, out-of-range binding exceptions, malformed JSON, schema reprepare, failed indexed-query recovery, concurrent readers, committed WAL updates, and writer-generation fencing.

The existing native/controller/SDK suites remain required. The benchmark also builds whenever native tests are enabled. Its no-argument smoke test uses a unique private temporary fixture, removes it afterward and asserts results without imposing a machine-dependent timing threshold.

## Reproducible measurements

Build `devbox-state-read-bench`, then run:

```text
devbox-state-read-bench NEW_FIXTURE_DIRECTORY 2000
```

The benchmark compares reuse off/on inside one executable, against equivalent synthetic fixtures. Eight alternating ABBAABBA trials include warm-up, per-call correctness oracles, median/p95/p99, mean, raw clock-floor observations and a four-thread contention case. Returned values contribute to a checksum. No production request, paid model call or external provider is involved. The timer is `std::chrono::steady_clock`; its observable floor is reported rather than treating sub-tick zero samples as zero execution cost.

Native timings include SQLite execution, result construction and the stated oracle checks. They exclude MCP/HTTP, Cloudflare, shell startup, child-process creation, filesystem durability for writes and model generation. Reaching a few microseconds in these hot queries is not a claim of microsecond end-to-end connector calls.

For a matched loopback measurement, `cpp-mcp/scripts/matched-benchmark.mjs` now accepts the optional `task` class. It creates and verifies a fixed checkpoint only in its newly created fixture. Existing default classes are unchanged. Use the same script, dependencies, sample count and fixture policy for both baseline and candidate binaries:

```text
DEVBOX_BENCH_STATE_BACKEND=sqlite
DEVBOX_BENCH_CLASSES=health,task
node cpp-mcp/scripts/matched-benchmark.mjs VERIFIED_BINARY NEW_OUTPUT_DIRECTORY
```

Those transport timings include the local fixed-route proxy, HTTP/MCP, serialization, coordinator access and result checking. The health class is a control. They must not be equated with the direct native-query measurements. Do not disable authentication on production or reuse its state for this fixture.

## Qualification policy

The initial isolated native pilot showed a lower median in every measured state-read category, including approximately 10.4 to 4.5 microseconds for a small record and 5.5 to 2.3 microseconds for materialized counts. These are pilot results, not final release claims. Final-head repeated measurements, exact binary identities, raw results, correctness outcomes and hosted checks are recorded in the performance PR discussion. Variation under shared workstation load must remain visible.

Alternative evaluated: prepare-per-call (reuse disabled). Retained candidate: fixed-slot, connection-local statement reuse. Rejected approaches include caching authorization/state results, weakening `synchronous=FULL`, removing ownership locks, changing machine-wide timers or priorities, and claiming remote-model or network latency from microbenchmarks.

No production deployment or resource-capacity tuning is part of this performance branch. Promotion still requires reviewed final source, all required CI/security/package checks, a signed exact tested artifact and the existing rollback/reconnection procedure.
