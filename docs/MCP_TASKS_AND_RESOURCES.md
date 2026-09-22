# Negotiated Tasks and evidence resources

The native backend implements the [2026-07-28 Tasks extension](https://github.com/modelcontextprotocol/ext-tasks/blob/main/specification/2026-07-28/tasks.md). The client must include `io.modelcontextprotocol/tasks: {}` in `params._meta["io.modelcontextprotocol/clientCapabilities"].extensions` on each relevant request. Prior negotiation is not enough. With SQLite enabled, `server/discover` advertises the extension.

Only eligible durable admissions are augmented: backend run creation, durable job submission, and research submission. Other tools retain ordinary results. Clients without the opt-in always receive the existing tool result and can use job/run status tools. No task-shaped result is sent before its principal-bound mapping is durable.

`tasks/get` returns the status, timestamps, `ttlMs: null`, a polling interval, and any final result or input requests. Underlying tool failures use a completed Task with an `isError` tool result, rather than incorrectly reporting a JSON-RPC task failure. Terminal snapshots are persisted and remain immutable. Private task and artifact views are never labeled publicly cacheable.

Backend approval waits appear as `input_required`. The form asks for an existing operator-issued grant ID; it cannot mint authority. Paused and uncertain runs expose the appropriate resume/cancel or reconciliation choices. `tasks/update` acknowledges accepted input and ignores unknown or already fulfilled request keys. `tasks/cancel` acknowledges the intent separately from eventual termination. Missing per-request Tasks support returns `-32021`; an unknown or unauthorized task ID returns `-32602`.

Modern clients can list and read `devbox://instructions/backend-v1` for the versioned workflow and `devbox://runs/{run_id}/artifacts/{sha256}` for exact run-owned artifacts. Artifact reads check authenticated principal ownership and the content digest and label the content as untrusted evidence. Polling is implemented; no task notification subscription support is claimed. Hermes UI remains out of scope.

The fixture exercises both the legacy response and the negotiated Task response for the same idempotent run; frontend reconnect; approval input and replay; durable jobs; terminal immutability; missing-capability and unknown-ID errors; and resource reads. It has passed against the real Windows and Linux native servers with two recorded model responses and an actual isolated effect. This is protocol/implementation evidence, not a live model quality claim.
