# C++ replacement implementation history

This document records the completion contract and successive development
milestones. Statements about incomplete modules below describe those milestones,
not the current implementation. The completed architecture, compatibility bounds,
certification and promotion requirements are in
[Native C++ runtime rewrite](CPP_RUNTIME_REWRITE.md).

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

## Search and integration milestone

Native search uses RE2 with bounded pattern memory, a 64 MiB per-file ceiling, bounded traversal state, and bounded result capture. It also preserves the existing ripgrep integration, including streamed JSON parsing and cancellation once the global match limit is reached. The legacy `HOST_SEARCH_BACKEND=js` setting selects the C++ native fallback for configuration compatibility. Host search remains a best-effort ripgrep-style interface; engine-specific regex constructs need a defined compatibility matrix.

Host inspection reads exact file bytes, distinguishes binary magic from text corruption, reports BOMs and line endings, and runs the PowerShell parser without elevation. Native file timestamps round to JavaScript millisecond precision. Tests cover valid and malformed scripts, invalid UTF-8, Unicode text, binary files, hashes and submillisecond timestamps.

C++ owns lifecycle orchestration, deferred retired-container cleanup, Guardian desired-state writes, and GitHub CLI authentication integration. Docker filesystem operations retain the existing container-side Python helpers as external integrations; those scripts are compiled into the C++ executable from checked-in generated assets. The C++ runtime does not launch Rust or require the development extraction script. Lifecycle tests exercise replacement and migration rollback against an isolated Docker fixture; live-container validation remains required.

Linux builds require a C++20 standard library with constexpr strings. On the Ubuntu 22.04 validation host, GCC 12 runtime components were already installed; the two missing C++ compiler/header packages are extracted into the task-owned dependency directory by `prepare-linux-gcc12.sh`. No system compiler selection or distro package installation is changed.

## Telemetry milestone

Usage logging has bounded 1,024-event writer queues, rotation, secret redaction, active invocation tracking, and visible write failures that recover only after a successful write. HTTP records omit authorization headers and query strings. The tool-scope refusal uses the existing MCP tool envelope and participates in the same invocation logging.

The performance sampler attaches to the actual HTTP I/O executor and records 10-second, one-minute, and five-minute latency windows. Process telemetry includes RSS, CPU time, Windows handle/thread counts, and requested-byte accounting for all standard C++ new/delete forms; direct allocations made by C libraries are outside the latter metric. A dedicated test introduces an executor stall and confirms the measured latency, then verifies orderly cancellation. All eleven Windows native suites pass at this milestone. Production integration and the remaining completion gates are still required.

## Standalone MCP dispatcher milestone

`devbox-mcp` now serves 40 implemented tools, including native execution, durable jobs, task checkpoints, file operations, search, inspection, lifecycle, GitHub integration and passive waits. Its runner and elevation worker are C++ executable modes. Capture tools remain excluded until implemented. Readiness remains false while the tool family is incomplete; `--parity-report` reports the unfinished completion gates and prohibits cutover.

The actual MCP SDK verifies the executable identity, binary transfer, a native detached job, duplicate-operation recovery, cancellation, and 32 concurrent passive waits. Twelve Windows CTest suites pass, including the real HTTP dispatcher and fault/recovery probes against an invalid job-store path. Embedded schemas and metadata match all 45 frozen Rust definitions in host, Docker, constrained and unlimited-transfer configurations, with the declared implementation name changed to C++.

Build provenance includes the compiler, source commit/ref/tree, dirty state, source-content fingerprint, build timestamp and actual executable SHA-256. Source metadata regeneration is a build dependency and also detects untracked source content. This is still a development milestone: native capture, installer replacement, live Docker/GitHub fixtures, complete result parity, cross-runtime recovery and platform certification remain required.

## Capture, installer and Linux validation

All 45 C++ tool handlers are implemented. Windows capture uses native GDI and WIC in a separate C++ worker process, preserving largest-window selection, process-tree fallback, black-frame rejection, queue bounds, retry policy and cancellation. Tests capture owned windows, validate the resulting JPEG and metadata, and verify that repeated capture releases GDI objects. Linux keeps the existing screenshot utility integrations. macOS discovers windows directly with CoreGraphics and retains the native screenshot command integration.

The bootstrap installer is now C++. It retains configuration options, comments and untouched values, supports native runtime staging, and invokes the existing launcher and Guardian integrations. The launcher verifies a clean source identity, executable digest and completion report before staging an immutable C++ candidate. It promotes that candidate only after health validation. An incomplete completion report prevents cutover. Runtime workers are excluded from managed-server ownership discovery.

Fifteen Windows native suites pass. All fifteen Linux suites also pass in release and address/undefined-behavior sanitizer builds. Linux validation exposed a temporary-lambda destruction problem in GCC coroutine expressions. Worker submission now completes in a separate expression before suspension, and a focused callback-ownership regression runs under sanitizers. The relevant compiler issue is documented in [GCC PR101243](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=101243). The local Linux build script preserves the selected compiler during CMake regeneration; the system compiler selection is unchanged.

The Windows result audit matches all 41 exercised calls when full-display captures are excluded; real capture has its separate native-image test. OAuth, Cloudflare OAuth, gateway, configuration, disconnect and durable-agent SDK audits pass. The full SDK smoke and Windows contention gates also pass, including explicit SSE cancellation, quota heartbeat visibility, 128 MiB output pressure and simultaneous watchers. Cross-runtime persistence, fresh-install packaging, managed startup, live Docker and the remaining platform matrix are still required. The production source and Rust executable remain at the frozen baseline; the service's own restart metadata may advance independently.
