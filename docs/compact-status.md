# Compact native status

Use `devbox_status_summary` for routine identity, readiness, admission, queue and
resource checks. It is implemented in C++23, requires the existing
`mcp:devbox:read` scope, and accepts an empty argument object. It remains
available while the operator drains new work.

The response retains the runtime state, complete build identity, guardian
health, execution-store health and disk warnings, degraded subsystem names and
their failure details, admission state, queue counts/capacities, performance
measurements, telemetry health, cached tool versions, and scoped-execution
availability. The explicit `ready` flag uses the existing readiness checks.
`observedAtUtc` is the response observation time; cached measurements keep their
own timestamps, ages and stale flags. It does not turn a cached measurement into
a fresh probe or guarantee that a subsequent action will succeed.

Queue counts and their sample timestamp, age and error state are copied under
one cache lock. The five-second availability bound is unchanged. The compact
path selects only the required queue fields before copying; the detailed path
copies its original complete snapshot once, instead of copying the enclosing
cache and snapshot multiple times.

A failed scheduler probe records its own `probeSampledAtUtc`, error and duration
without changing the retained queue snapshot's `sampledAtUtc`. Repeated failures
therefore cannot keep old queue counts inside the five-second freshness bound.
A first failed probe has no queue sample. This also corrects the existing
readiness/status cache path, which previously timestamped a retained snapshot
with the time of the failed attempt.

The compact action avoids copying and serializing the full tool catalogue and
does not construct successful background-worker histories, and avoids reading the detailed startup
and maintenance record files. It uses compact JSON for the model-visible text.
Retrieve `devbox_status` for those details, individual lease owners and complete
diagnostics, or `devbox_capabilities` for authoritative tool schemas. The old
action's arguments, default behavior and detailed output are unchanged.

Contract version 13 advertises this as a native extension. No optional user
language runtime or new production dependency is required. The repository's
frozen legacy host/Docker schemas remain unchanged.

The native engine test verifies actual MCP dispatch, build/schema identity,
health/warning/isolation fields, queue and cache freshness, text/structured-data
agreement, argument validation, and drain admission. The isolated lifetime
benchmark reports 100 detailed and 100 compact status responses, including
complete wire bytes and C++ allocation counters. Counter coverage excludes
direct C-library allocations and includes the benchmark client; measured bytes
are not an exact model-token count. Compare retained qualified runs before
claiming a performance improvement. Supply `--compact-first` after the new fixture
directory to reverse the two status phases. Use fresh processes with alternating
phase order, retain source/binary identity and all readiness/shutdown gates, and
compare both model-visible text bytes and complete response bytes. No production
request or cache is changed by the benchmark.

## Measured Windows fixture

Four fresh MSVC Release processes on 30 October 2026 used phase order detailed/
compact, compact/detailed, compact/detailed, detailed/compact. Each process ran
100 requests per status action against its own loopback server and private
durable state, with the same compiled binary. The source was
`edfad3872ee32d83c62275c6792da12b16f57f9d`; the benchmark executable SHA-256 was
`fbaf758464b0d03a037706b30c0f646283547df2e48f95a33c083c79eb050771`.

Medians across the four trials, per 100 calls:

| Metric | Detailed status | Compact status | Reduction |
| --- | ---: | ---: | ---: |
| Complete response bytes | 8,774,205 | 1,365,876 | 84.4% |
| Model-visible text bytes | 4,920,352.5 | 641,738 | 87.0% |
| C++ allocation calls | 2,787,542.5 | 514,411 | 81.5% |
| C++ requested bytes | 328,919,084.5 | 63,606,519.5 | 80.7% |
| Loopback elapsed time | 483.4 ms | 188.8 ms | 60.9% |

The two byte measures are distinct: the MCP response includes both text and
structured content, while text bytes cover the returned text fields only.
Neither is a tokenizer count. These measurements compare the two actions in
the candidate, not two production deployments. Allocation counters include
the benchmark HTTP client and background work and exclude direct C-library
allocations. Timing comes from a shared workstation; it is not a model or
internet latency claim, or proof that the entire server is leak-free.

All four trials finished ready with zero live HTTP connections, and the
coordinator's recorded process instance was verified exited after shutdown.
The initial harness attempt incorrectly expected `coordinator.json` to be
deleted; that file is intentionally retained. Its process had exited. That
attempt is excluded from the four qualified trials. The corrected harness
checks actual process identity and exit, and does not remove state files to
make the check pass.

Windows engine/background-state tests and Linux GCC 12.3 engine/background-state/
core tests passed at the measured source. Full hosted qualification and
production deployment are separate gates. The individual phase measurements,
source identity, resource checks and process-exit receipts are retained in
[the benchmark record](benchmarks/compact-status-20261030.json).
