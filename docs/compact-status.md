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
