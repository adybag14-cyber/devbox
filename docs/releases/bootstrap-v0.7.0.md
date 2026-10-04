# Devbox native core 0.7.0

This release adds a C++23 management CLI to the MCP executable and a distinct
`devbox-core-TARGET` archive for every qualified target. The full bundles remain.

- Native initialization, foreground/detached supervision, status, stop/restart,
  immutable candidate staging, signed promotion and rollback.
- No Node/npm/Git dependency for native core startup or management. Signed
  installation/promotion uses GitHub CLI only to preserve the established
  exact-artifact attestation trust policy.
- Process birth/executable identity checks, duplicate suppression, independent
  heartbeat, bounded logs/restart backoff, journaled handoff and owned companion
  lifecycle. The ChatGPT-facing status tool reports native supervisor health.
- Drained replacement preserves durable state and refuses busy/uncertain work.
  State schema/protocol changes require a separately reviewed migration.
- Native installer path and generated Windows/systemd/launchd service definitions;
  the explicit source-checkout/TUI workflow remains available for compatibility.
- Static Linux C++/GCC support runtime and import inventories checked against the
  actual release binaries. Clean minimal-container and lifecycle regressions run
  alongside existing platform/security/SDK/desktop qualification.

See [core usage](../CORE_SERVER.md) and [runtime requirements](../RUNTIME_DEPENDENCIES.md).
Production deployment is a separate step. Building or publishing the release does
not replace any running Windows or Oracle service.
