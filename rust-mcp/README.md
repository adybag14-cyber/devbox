# Rust MCP replacement (Draft migration)

This directory contains the native Rust replacement for `src/server.js`.

It is intentionally developed in parallel with the production JavaScript MCP. The Windows launcher continues to start the JavaScript implementation until the Rust replacement reaches full technical and feature parity and passes the same cross-platform/E2E contracts.

## Review gate

The pull request for this migration **must remain Draft** until all of these are true:

1. All 37 current MCP tools are implemented with compatible names, inputs, outputs, cancellation, and failure semantics.
2. `/`, `/healthz`, root MCP POST, `/mcp`, SSE/JSON transport behavior, protocol versions, CORS/host validation, and disconnect cancellation match the JS service.
3. `none`, `demo-oauth`, and `cloudflare-access` authentication modes are feature complete, including OAuth discovery/resource metadata and persisted state behavior.
4. Host and Docker runtimes both pass parity tests on Windows, Linux, macOS, Android/Termux, and the official Termux Docker E2E.
5. Async jobs, weighted scheduling/watch pool, log rotation/retention, orphan reconciliation, large-file operations, native process cancellation, search, and screen capture pass destructive/fault-injection tests.
6. Guardian/startup scripts can preflight and hot-swap the Rust binary with rollback to the JS implementation.
7. Benchmarks demonstrate no material regression and the intended reductions in startup time/RSS/tail latency.
8. The JavaScript implementation remains available as rollback until a separate post-migration cleanup PR.

Only after those gates pass should the PR be converted from Draft to Ready for Review so CodeRabbit reviews the completed replacement.

## Current milestone

The first vertical slice contains:

- official `rmcp` Streamable HTTP server transport
- `/healthz`
- root metadata
- root POST and `/mcp` transport routes
- shared `.env.runtime`/`.env` configuration semantics
- cancellation-aware `devbox_wait`
- cancellation-aware `devbox_wait_for_file`
- initial `devbox_status`
- `host_status` and `windows_host_status`
- exact-byte `windows_host_read_large_file` and `windows_host_write_large_file` with SHA-256 verification
- configurable MCP transfer ceilings matching the JavaScript service
- explicit parity report for the 37-tool target contract
- weighted execution scheduling with reserved interactive capacity and a separate passive-watch pool
- persistent detached-job state, orphan reconciliation, PID-reuse-safe cleanup, retention, and rotating logs
- structured host/Docker runtime execution with PowerShell 7 preference on Windows
- `devbox_run_program` direct allowlisted execution
- `devbox_run_program_start`, `devbox_job_status`, `devbox_job_logs`, and `devbox_job_cancel`
- synchronous `devbox_exec` / `devbox_exec_readonly` plus detached `devbox_exec_start`
- live JavaScript MCP SDK smoke coverage, including detached success/cancellation lifecycle, enforced in the Windows/Linux/macOS CI matrix

The Rust shadow currently advertises **15 of the 37** target tools. No production launcher points at this binary yet.
