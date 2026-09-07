# Complete C++ replacement

Baseline: `cd8803c81ad3a14ec3b3fa2afa0d08256975b1e9`, contract version 2, 45 tools. The Rust runtime and bootstrap installer are the reference implementations during this port. Production remains pinned to that existing Rust deployment until the C++ replacement is complete and validated.

## Completion contract

The replacement must execute its HTTP/MCP transport, authorization, host/Docker commands, detached runners, scheduling, cancellation, files/search, task/operation receipts, capture, monitoring and installation logic in C++. It must not call the Rust executable or expose unimplemented tool handlers as supported features. Guardian and external host programs remain integrations.

Both the MCP runtime and Rust bootstrap installer are in scope. Existing job requests, statuses, receipts, task checkpoints and OAuth state must remain readable. The 37 legacy tools and eight native durable-agent tools retain their input/output contracts. C++20 is the portable language baseline, with pinned native dependencies and native platform process/filesystem backends.

## Module map and acceptance

| Area | Rust authority | C++ destination | Required evidence |
| --- | --- | --- | --- |
| Configuration, outputs, identity | config, result, output, provenance | shared core | Environment/schema/result fixtures, clean-build identity |
| HTTP/MCP and scopes | server, gateway, request_control, schema_parity | transport and dispatch | SDK, protocol revisions, cancellation, all 45 schemas |
| OAuth/JWKS | oauth | auth | PKCE, registration, token rotation/revocation, bounded JWKS stalls |
| Host/process control | runtime, process, windows_* | process and runtime | Native Windows/POSIX execution, pipe bounds, ownership, deadlines |
| Scheduling/jobs | execution, jobs, job_manager, job_runner, job_logs | scheduler and jobs | Cross-process queue compatibility, dedup, admission, restart, cancellation, retention |
| Files and task state | files, atomic_file, task_store, docker_files | storage | Byte exactness, conditional writes, ACLs, alias policy, durable retries |
| Search, inspection, lifecycle | search, host_inspect, lifecycle, github_auth | tools | Functional parity in host and Docker modes |
| Capture | capture, windows_capture, posix_capture | capture | Real images, process-window selection, timeout/cold-start checks |
| Monitoring | background, performance, usage, incident_task, quota_task | monitoring | Bounded queues, rotation, health, metrics and failure visibility |
| Installer and deployment | bootstrap, managed launchers | cpp-bootstrap and launcher integration | Fresh install, immutable C++ candidates, Guardian and rollback |

Production changes are a separate final phase. Until every row is complete and cross-platform gates pass, no production checkout update, configuration edit, service restart, Rust binary replacement or tunnel change is permitted for this rewrite.

## Native backend milestone

The C++ core currently implements native processes, host/Docker shell adapters, conditional atomic writes, binary file transfer, task checkpoints, directory listing, weighted scheduling, detached job runners, operation receipts, heartbeat reconciliation, log rotation, retention and quota. Six Windows test executables cover these modules, including actual child processes and concurrent filesystem access. This milestone does not provide a complete MCP server and does not authorize a production cutover.

Windows journal reads share delete access. Internal journal and queue replacements use `FileRenameInfoEx` with POSIX rename semantics so existing readers keep their previous snapshot while new readers open the replacement. Unsupported filesystems use a bounded classic-rename fallback. User-file replacements continue to use `ReplaceFileW` to preserve destination permissions, with a regression that keeps the previous file open while replacing it. Both behaviors follow the [Microsoft file rename contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information).

The POSIX process backend owns a process group for each direct invocation. Detached runners handle termination signals and cancel their separately owned command group. A submitting process can exit without terminating an admitted detached job. Receipt retention is independent of normal job-result retention, and completed requests cannot be executed again by relaunching the C++ runner.

## Authentication and transport milestone

The native transport uses two Asio I/O threads, bounded blocking work queues, per-request SSE heartbeats, and cancellation scoped by authorization digest, session, peer address, user agent and typed request ID. Client disconnects cancel the corresponding invocation. Windows listeners request exclusive ownership before binding. A test backend exercises these properties without advertising unfinished production tools.

OAuth state retains the existing tuple-array JSON format. Native C++ implements registration, PKCE, authorization-code consumption, refresh rotation, revocation, narrow tool scopes and Cloudflare RS256 verification. JWKS requests have a ten-second deadline and 256 KiB response bound; cache refresh does not hold the token-state lock. URL validation uses the pinned Ada 4.0.0 WHATWG parser. Windows tests cover malformed metadata, redirect binding, persisted state, signature/issuer/audience/expiry failures, stalled key retrieval, HTTP authorization and local gateway restrictions. The actual MCP JavaScript SDK 1.30.0 also completes initialization, SSE calls, cancellation and 24 concurrent passive waits against the C++ test transport.

These checks validate the transport and authentication modules. Full tool dispatch, production monitoring, installer replacement and platform certification remain mandatory before completion.
