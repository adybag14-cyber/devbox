# Native Rust Devbox MCP

The production service is the Rust Axum/Tokio/rmcp implementation. Guardian supervises it on Windows, Linux, macOS and Termux. Windows production uses the elevated Guardian profile. The JavaScript service remains a legacy compatibility implementation.

Version 0.2 exposes contract version 2: the 37 compatible legacy tools plus eight native APIs for durable job submission/discovery, versioned task state, atomic file writes and capability inspection.

See [the agent runtime contract](../docs/AGENT_RUNTIME.md) for supported versions, exact retry/cancellation semantics, scheduling and admission bounds, source provenance, and connector refresh requirements. See [Guardian](../docs/GUARDIAN.md) for operational ownership and supervision.

Managed production startup builds and validates the locked Rust candidate before stopping the existing service. It requires clean committed source, records the source tree and binary hash, and verifies embedded provenance before reusing a candidate. DEVBOX_MCP_IMPLEMENTATION=rust selects Rust explicitly; production rollback should use a previously validated Rust candidate.

The CI matrix checks Rust formatting, strict Clippy, the declared MSRV, dependency audit, unit tests, HTTP/auth/disconnect behavior, the legacy schema/result contract, native durable APIs, and Windows/Linux/macOS/Termux runtime integration. Local probes use isolated ports and directories; they do not certify a production deployment by themselves.
