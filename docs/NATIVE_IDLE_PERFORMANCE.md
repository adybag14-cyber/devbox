# Native idle-work and memory audit

The 22 October 2026 audit measured the deployed 0.9.3 C++23 runtime on Windows and Oracle before changing it. Windows' previous seven-hour run and Oracle's 21-hour run had roughly 2 MB of live requested C++ allocations after warm-up. A five-minute Windows sample nevertheless allocated and freed about 1.99 GB in total. Most of that activity occurred in periodic bursts; cumulative allocation is not retained memory.

The complete-server fixture separately exercised 400 loopback HTTP requests and observed zero remaining connections and zero request/response budget reservations afterward. These observations do not certify the absence of all leaks: the global C++ allocator counters do not cover C libraries using `malloc`, resident pages can be swapped or shared, and the longer production sample overlapped ordinary host activity and audit builds.

## Maintenance candidates

Previously, each maintenance cycle read every historical job and rewrote its usage sample, including jobs whose artifacts had already been pruned. Historical job identities must survive pruning for discovery and idempotency, so this overhead grew with the lifetime job count.

The native SQLite store now maintains a small derived `job_maintenance_candidates` table with a `(principal,id)` index. Jobs leave that table only when both authoritative records agree: the job is terminal with a boolean `artifacts_pruned=true`, and its same-principal usage record is absent, zero-byte and compacted. C++ performs these checks using the existing JSON implementation. SQLite's optional SQL JSON functions remain unnecessary.

The table is a work index, not permission to delete anything. The existing producer identity, termination acknowledgement, retention rules, pruning intent and revision-checked accounting transaction still govern filesystem effects. Normal job lists, counts, gets and operation receipts retain every historical identity. Active jobs, malformed pruning flags, missing charges and changed charges remain candidates.

This is an additive schema-v2 extension. Ordinary SQL triggers conservatively requeue a job whenever its job or usage record changes, including writes by an older executable after rollback. The new C++ writer retires a fully settled pair within that same transaction. The first open of an older database queues existing identities for one bounded reconciliation pass; it does not parse all job payloads during startup. Each page still observes the original 64-record maintenance bound and the existing response-byte budget.

The optional internal query hint is serialized through the existing authenticated state protocol. An older coordinator can return extra candidates, preserving the original safe scan behaviour. The new indexed query uses an explicit principal/index path and a separate prepared-statement slot. It does not cache result rows, principal decisions or writer authority.

The query puts the small candidate index outside the record lookup using SQLite's documented [CROSS JOIN ordering](https://www.sqlite.org/optoverview.html#manual_control_of_query_plans_using_cross_join). This gives the regression tests a bounded-work contract independent of stale planner statistics. Usage-record prefetch targets retained terminal charges; active, missing or shifted IDs retain the existing per-ID fallback and revision comparisons.

## Small operational reads and timestamp validation

`read_file_range` uses a 4 KiB stack scratch buffer for small files and allocates a 64 KiB transfer buffer only after reading enough data to justify it. Offset, EOF, byte-limit, binary-content and error semantics are preserved. This removes the fixed 64 KiB heap allocation from frequent small journal reads without replacing large-file transfers with permanently small chunks.

UTC parsing now requires both time separators and at least one fractional digit after a decimal point. The previous parser accepted malformed forms such as `12-53-00Z` and `12:53:00.Z`. Existing valid offset, lowercase and extended-fraction support remains covered.

## Reproduction and correctness checks

Build the optional benchmarks with `DEVBOX_BUILD_BENCHMARKS=ON`:

```text
devbox-idle-components-bench NEW_ABSOLUTE_FIXTURE_DIRECTORY
devbox-server-lifetime-bench NEW_ABSOLUTE_FIXTURE_DIRECTORY
devbox-maintenance-workers-bench NEW_ABSOLUTE_FIXTURE_DIRECTORY 64
```

Every fixture requires a new directory. The maintenance corpus retains 512 historical identities, with 64 retained artifact directories and 448 settled tombstones. Before this change, the expected scan count is 512; afterward it is 64. Correctness checks retain exact artifact-byte accounting, history count and retained-directory count. Report warm-up separately from measured cycles and preserve raw clock and allocator observations.

Local Windows measurements, three measured cycles after one warm-up:

| Synthetic maintenance pass | Baseline | Candidate |
|---|---:|---:|
| Rows visited / pages | 512 / 8 | 64 / 1 |
| Wall time, milliseconds | 275.436 / 262.006 / 258.565 | 54.101 / 53.135 / 50.231 |
| Parent allocation calls | 246,718 / 246,710 / 246,706 | 34,436 / 34,436 / 34,436 |
| Parent bytes allocated | 35,051,473 / 34,920,096 / 34,854,404 | 4,642,281 / 4,642,281 / 4,642,281 |
| Historical identities / retained directories | 512 / 64 | 512 / 64 |

The same small JSON-read component allocated 13,229,200 bytes across 200 baseline reads and 114,200 bytes after the scratch-buffer change. These are isolated component measurements, not a claim that every tool call or website is that much faster. Live post-deployment observation and hosted platform/sanitizer qualification remain distinct evidence.

`state-maintenance` tests bounded indexed work past 2,000 settled tombstones, pagination, principal isolation, active/malformed markers, ordinary history, charge invalidation, additive upgrade and read-only compatibility. `indexed-maintenance` retains crash, cancellation, revision-conflict and pruning-accounting coverage. Core tests cover malformed timestamps and small/binary reads across chunk boundaries. Platform qualification and sanitizer checks remain separate gates from production observations.
