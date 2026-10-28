# Devbox C++23 0.9.13

Background status updates now keep counters, timestamps and errors in typed native
state and construct the existing ordered JSON only when it is requested. Snapshot
construction reserves its output storage and avoids redundant searches for fields
and worker names that are already unique. Field order, nulls, failure/recovery
history, locking and detached snapshot ownership are preserved. The controlled
Windows 14-worker fixture reduced allocations per snapshot from 693 to 131 and
median component time from 34.06 to 7.44 microseconds. Attempt/success updates in
that fixture allocate no temporary storage. See
[background state measurements](../background-state-performance.md).

The scalar JSON helpers use their bounded string-view keys directly, eliminating
temporary key allocations. Empty, UTF-8 and embedded-NUL keys, substring views,
missing fields, defaults and type checks have regression coverage. The controlled
long-key fixture reduced one allocation per lookup to zero. These component
results do not establish a whole-server latency or CPU improvement. See
[JSON key measurements](../json-key-view-performance.md).

After a transient health failure, the native supervisor now clears its stale
unhealthy phase and current health error when the same frontend passes its
identity-bound readiness checks. It retains operator acknowledgement history and
does not replace a frontend whose work was preserved by a refused drain. The Linux
lifecycle regression stalls only its verified fixture process, observes the
refused drain and continuing heartbeat, resumes that process and verifies recovery
without a generation change.

Performance capture releases copied sample histories and percentile workspace
before measuring live process allocations. This corrects measurement overhead
being reported as retained server memory; a lower reported value from this change
is not itself proof of an application memory reduction. Browser startup results
also expose bounded microsecond timings for session setup phases, supporting
diagnosis of target creation, page enablement and network-policy setup without
changing their ordering, deadlines or security policy. See
[browser startup timings](../browser-startup-timings.md).

The full-server benchmark now requires a responding durable coordinator and
checks maintenance/quota errors, so a readiness-only pass cannot silently qualify
a degraded fixture. Optional benchmark cleanup uses the coordinator's authenticated
protocol and stored process birth identity.

Offline installers, user-level/portable Windows packages, separate lean/core
archives and the verified optional ripgrep fallback continue from
[0.9.12](bootstrap-v0.9.12.md). This release adds no installed runtime dependency.
Node, Python, a Docker engine, Chromium and Cloudflare Tunnel are not bundled.

Release qualification and production activation remain separate. Deployments must
verify their source-bound signed artifacts, preserve active work through the
native maintenance gates and validate the updated live endpoint and website agent.
