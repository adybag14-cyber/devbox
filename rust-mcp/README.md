# Rust MCP replacement (Draft migration)

This directory contains the native Rust replacement for `src/server.js`. The replacement now implements the complete 37-tool MCP contract and is the default implementation in the cutover launchers on this migration branch. The JavaScript server remains available through `DEVBOX_MCP_IMPLEMENTATION=js` as the explicit rollback path.

The production service is not considered migrated merely because the launcher defaults have changed in the branch. The shared/live checkout stays on the previously validated service until the cutover commits, cross-platform runtime E2E, rollback drill, Guardian ownership, and final live verification are green.

## Review gate

PR #25 must remain **Draft** until all of these are true:

1. All 37 MCP tools retain compatible names, schemas, outputs, cancellation, error, and platform semantics.
2. `/`, `/healthz`, root MCP POST, `/mcp`, SSE/JSON transport behavior, protocol versions, CORS/host validation, body limits, and disconnect cancellation match the JavaScript service.
3. `none`, `demo-oauth`, and `cloudflare-access` authentication modes remain feature complete, including discovery/resource metadata and persisted state behavior.
4. Host and Docker profiles pass the parity oracles and runtime E2E on Windows, Linux distributions, macOS, and Android/Termux.
5. Async jobs, weighted scheduling/watch capacity, log rotation/retention, orphan reconciliation, large-file operations, native process cancellation, search, and screen capture retain their regression coverage.
6. Portable and managed launchers preflight the locked Rust release before replacement, preserve implementation/PID ownership metadata, and can hot-swap Rust -> JavaScript -> Rust without cross-checkout process ownership mistakes.
7. Guardian, startup, rollback, and final live health/MCP/Chrome/authenticated-GitHub checks pass after the shared cutover.
8. The JavaScript implementation remains available as rollback until a separate post-migration cleanup PR.

Only after those gates pass should the PR be converted from Draft to Ready for Review.

## Current implementation state

The Rust MCP now provides:

- all **37 of 37** target MCP tools
- exact JavaScript-to-Rust input/output schema parity for host and Docker profiles
- exact metadata parity, with platform-specific rendering for Windows, Linux, and macOS
- a deterministic **41-call** JavaScript-to-Rust observable-result oracle in CI, plus broader SDK interoperability coverage across all tools
- Streamable HTTP transport, root and `/mcp` routes, `/healthz`, gateway/CORS/PNA/body-limit behavior, and disconnect cancellation
- `none`, demo OAuth, and Cloudflare Access authentication behavior
- cancellation-aware waits, files/search, exact-byte large-file operations, hashing/base64 behavior, and JavaScript-compatible errors
- synchronous and detached host/devbox execution, persistent jobs, cancellation, orphan recovery, weighted execution slots, passive-watch capacity, rotating logs, and retention
- Windows host aliases, PowerShell behavior, GitHub authentication helpers, screen capture, and platform-specific compatibility semantics
- Windows transient execution-slot contention hardening, including repeated concurrent-heavy regression coverage

## Cutover behavior on this branch

`DEVBOX_MCP_IMPLEMENTATION` selects the implementation:

- unset or `rust`: build/preflight the locked Rust release and launch it
- `js`: launch the retained JavaScript rollback implementation

Both the portable Node launcher and the Windows managed PowerShell lifecycle preflight the selected replacement **before** stopping an existing owned MCP. The managed launcher records `run/mcp.implementation`, validates checkout-local ownership, applies the generated `.env.runtime` values authoritatively to the spawned child, and restores the parent process environment immediately afterward.

The portable launcher has been exercised on isolated ports with both implementations: Rust and JavaScript each reached healthy status and passed the 22-tool platform runtime E2E before a clean stop. The Windows managed launcher is gated separately by cross-checkout ownership and runtime-environment regression tests; its isolated Rust/JavaScript hot-swap drill must be rerun successfully before any live cutover.

## CI and remaining deployment gates

The Rust parity workflow runs on Windows, Linux, and macOS with formatting, strict Clippy, tests, build, schema parity, observable-result parity, and SDK smoke gates. The platform runtime workflow is being migrated to require Rust/Cargo for source installs and to assert `implementation: rust` on native macOS, Linux distribution containers, and Termux.

Before the production switch, the cutover commits still need to pass the full platform matrix, be integrated into the shared replacement branch, and complete a controlled live Rust cutover plus an explicit JavaScript rollback-and-return-to-Rust drill.
