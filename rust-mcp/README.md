# Native Rust Devbox MCP

This directory retains the Rust Axum/Tokio/rmcp reference. The default native
service is now the [C++ implementation](../docs/CPP_RUNTIME_REWRITE.md), supervised
by the existing Guardian. The SDK regression scripts in this directory accept
`DEVBOX_MCP_TEST_BINARY` so the same compatibility assertions validate C++.
The Rust implementation remains an explicit legacy selection.

Version 0.2 exposes contract version 2: the 37 compatible legacy tools plus eight native APIs for durable job submission/discovery, versioned task state, atomic file writes and capability inspection.

See [the agent runtime contract](../docs/AGENT_RUNTIME.md) for supported versions, exact retry/cancellation semantics, scheduling and admission bounds, source provenance, and connector refresh requirements. See [Guardian](../docs/GUARDIAN.md) for operational ownership and supervision.

Managed production startup builds and validates the locked Rust candidate before stopping the existing service. It requires clean committed source, records the source tree and binary hash, and verifies embedded provenance before reusing a candidate. DEVBOX_MCP_IMPLEMENTATION=rust selects Rust explicitly; production rollback should use a previously validated Rust candidate.

The CI matrix checks Rust formatting, strict Clippy, the declared MSRV, dependency audit, unit tests, HTTP/auth/disconnect behavior, the legacy schema/result contract, native durable APIs, and Windows/Linux/macOS/Termux runtime integration. Local probes use isolated ports and directories; they do not certify a production deployment by themselves.
