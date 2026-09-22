# State exports and recovery

The native operator CLI has three distinct operations. None is a model-facing MCP tool.

- `devbox-mcp --state-export NEW_REPORT.json` opens the configured SQLite store read-only and writes an allowlisted diagnostic summary. It omits identifiers, record payloads, prompts, arguments, task state, events, logs, artifacts and credentials. Counts are individually sampled, not a transactional report.
- `devbox-mcp --private-state-snapshot NEW_DIRECTORY --include-private-state` explicitly includes private database payloads. It uses SQLite's online backup API, validates the resulting database, records its SHA-256, and creates an owner-only destination. It never replaces an existing directory. Provider configuration files, broker credential files and external artifacts are not bundled. User-entered sensitive content inside database records can be present; this is a private snapshot.
- `devbox-mcp --restore-state-snapshot SNAPSHOT NEW_DIRECTORY` verifies the manifest and digest and restores metadata into a new directory. Ordered events and operation identities are preserved. The result is fenced against writable opens and automatic replay. A snapshot can predate an external effect, so copying an older database is not a safe way to resume a live workload. Existing live state is never overwritten.

A `recovery-fenced.json` marker makes snapshots inspection/reconciliation artifacts. Read-only access remains available. Reconcile uncertain outcomes against the current operation receipts and external artifact storage before establishing replacement live state. Removing the marker to retry old operations is not a supported recovery procedure. Normal release rollback keeps the authoritative live state in place and changes only a compatible, verified runtime artifact.

The private snapshot method is also available to the authenticated local coordinator's native client interface. It is not advertised to remote MCP clients. No API keys or paid services are required.

Validation covers a credential canary omitted from the default export, private payload inclusion only in the explicit snapshot, preserved records/events, later effects absent from the older snapshot, refusal to overwrite live state, refusal of writable recovery, and tampered-manifest rejection. The Windows CLI smoke additionally checked the mandatory private-data flag.
