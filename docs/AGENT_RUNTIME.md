# Native Rust agent runtime

Devbox's production MCP implementation is Rust. Guardian is the Node supervisor responsible for availability and managed Windows elevation. The Rust service retains 37 legacy-compatible tool names and adds eight native tools; the JavaScript implementation is a legacy compatibility oracle, not the production profile described here.

## Supported production profile and source identity

Windows production uses `DEVBOX_MCP_IMPLEMENTATION=rust`, `DEVBOX_RUNTIME_MODE=host`, and the Guardian scheduled tasks. Node 24 is the certified supervisor/tool-client version; the dependency-light launcher/Guardian minimum remains Node 18. The server declares Rust MSRV 1.88.0 and the repository/installer pin Rust 1.91.1. The standalone setup CLI declares its separate MSRV 1.74. These version contracts are checked by `scripts/check-runtime-contract.mjs`.

Managed production startup rejects dirty tracked source, untracked Rust build inputs, and unknown Git provenance before stopping the current MCP. Versioned binaries and `run/bin/current-rust.json` record the actual commit, Git tree and binary SHA-256. `--build-info` reports embedded source identity and whether source was dirty. Legacy manifests lacking source provenance are rebuilt instead of reused. Build metadata cannot be overridden by a stale `DEVBOX_BUILD_GIT_SHA` environment value.

Ordinary `devbox_exec` and detached shell jobs honor `HOST_SHELL` and inherit the MCP token. Explicit Windows `host_exec` remains the administrative PowerShell interface. Guardian runs the production Rust service elevated, without per-command UAC prompts. Use a verified previous Rust candidate for operational rollback.

CMD inline commands are limited to 8,000 UTF-16 units and return a clear error before launch when oversized; save longer commands to a script file. Ordinary PowerShell retains the configured-shell output behavior, while the administrative PowerShell interface retains its quiet/normalized output contract.

## Native tools

| Tool | Purpose |
| --- | --- |
| `devbox_capabilities` | Contract version, schema hash, tool names, source identity and effective limits; pass `tool_name` to inspect one complete schema |
| `devbox_file_state` | Complete file SHA-256 and byte length before a conditional write |
| `devbox_write_file_atomic` | Atomic replacement or append with previous-content/offset checks and equivalent-result retry detection |
| `devbox_job_submit` | Durable task/operation identity and duplicate-safe submission |
| `devbox_job_list` | Job discovery with task/status filters and bounded job-ID pagination |
| `devbox_task_get` | Read persisted task state and revision |
| `devbox_task_put` | Atomically update task state using an expected revision |
| `devbox_task_list` | Discover task IDs and revisions after a lost conversation or reconnect |

File state, atomic file writes and durable job submission operate on the host runtime. Task metadata is stored by the service under `run/tasks`; it is data and never grants execution permissions. OAuth read/exec scopes apply to the corresponding native tools. The default broad `mcp:tools` scope remains compatible. Task and operation IDs use 1-80 lowercase ASCII letters, digits, hyphens or underscores, avoiding platform-dependent case collisions.

## Submission and recovery

Submit one executable with structured arguments:

```json
{
  "task_id": "release-check",
  "operation_id": "build-001",
  "label": "Build candidate",
  "program": "node",
  "args": ["build.mjs"],
  "working_dir": "C:/workspace/project",
  "timeout_seconds": 7200,
  "resource_class": "heavy"
}
```

Exactly one of `program` or `command` is required. `args` and `input` belong to program jobs. Retrying an identical task/operation request returns the same job with `replayed: true`; changing its payload returns `OPERATION_CONFLICT`. The submission receipt is flushed before launch. If a server dies at an uncertain point before a complete job record exists, retry returns `submission_unknown` and does not execute again. This gives at-most-once launch, not an automatic replay guarantee. Inspect the task and artifacts before deliberately choosing a new operation ID.

Receipts outlive ordinary job/log retention. A retry after the retained job is gone returns `result_expired` without re-execution. Corrupted/unreadable existing job state returns `JOB_STATE_UNAVAILABLE` rather than being mislabeled as normal expiry. Receipt capacity is bounded and new operation IDs are refused when it is full; receipts are not silently evicted. Archiving operation receipts is an explicit operator action that ends their deduplication protection.

`devbox_job_list` supports `task_id`, `statuses`, `limit` (1-100) and `cursor`. Ordering is by the authoritative job directory ID. A cursor is returned only when another matching page exists. Each listing is a fresh view; restart pagination when concurrently inserted jobs must be discovered. Discovery has a five-second deadline and scans at most 10,000 job directories; retention should keep the store below that bound.

Save the returned job IDs and next action with `devbox_task_put`, using `expected_revision: 0` to create a task. Later writes must use the current revision. Stale updates conflict; an identical retry returns the committed revision. Task state is capped at 65,536 bytes and the store at 10,000 records. Store large artifacts separately and retain their paths and hashes in the task state. The agent remains responsible for plans, dependencies, human approvals and artifact verification.

## Atomic file semantics

Get the previous digest/length through `devbox_file_state`. Supply that digest as `expected_file_sha256`, or `missing` for an absent target. Append also requires `expected_offset_bytes`. The service validates the previous state; if the exact requested resulting state is already present, it returns `replayed: true` without writing again. Append replay detection checks both the previous prefix and suffix, preventing duplicate bytes after an acknowledgement is lost.

Existing host-runtime text and large-file writes also stage complete replacement contents. Staging is flushed before atomic replacement; Windows replacement preserves the destination ACL. An overwrite or append leaves a complete old or new file after a process crash. Atomic append copies the previous contents using bounded memory, so its I/O cost grows with file size. Use an appropriate checkpoint size and separate large streaming artifacts.

The service uses 256 fixed lock stripes and an OS lock protocol shared by cooperating processes under the same OS account. It never accumulates a lock object per filename. Blocking atomic I/O is limited to two workers, and a worker retains its permit even if the caller disconnects. A disconnected request may have committed: inspect/retry its intended state rather than assuming it did nothing.

Persistent locks live under the account profile's `.devbox/atomic-locks-v2` directory (under `LOCALAPPDATA` on Windows and `HOME` on Unix). Unix directories must be owned by the current account and private; symlinks, publicly accessible lock files and hard-linked lock files are rejected. The shared system temporary directory is not used for this lock protocol.

Valid symlinks resolve to their target. Dangling links and multiply hard-linked targets are rejected before replacement, because replacing a name cannot atomically update every hard-link alias. Cooperative API writers are serialized; arbitrary external programs must honor the same coordination or remain outside the compare-and-swap guarantee. The service also checks for changes during staging, but no ordinary rename API can provide a transaction with an uncooperative external editor.

## Bounds and cancellation

Execution slots and runner admission are separate bounds:

| Setting | Default | Meaning |
| --- | --- | --- |
| `MCP_EXEC_MAX_CONCURRENT` | 6 | Weighted execution slots |
| `MCP_EXEC_RESERVED_INTERACTIVE` | 1 | Reserved interactive capacity |
| `MCP_EXEC_HEAVY_CAPACITY` | 4 | Slots eligible for heavy work; the owner's production override is 5 |
| `MCP_EXEC_HEAVY_WEIGHT` | 2 | Default weight per heavy job |
| `MCP_WATCH_MAX_CONCURRENT` | 4 | Independent passive-watch pool |
| `MCP_JOB_MAX_ACTIVE_RUNNERS` | 16 | Total running plus queued runners, including legacy starts |
| `MCP_JOB_MAX_RUNNERS_PER_TASK` | 8 | Running plus queued durable jobs for one task |
| `MCP_JOB_MAX_OPERATION_RECEIPTS` | 10000 | Durable submission identities retained before admission refuses new IDs |

Heavy capacity is weighted capacity, not a promise of five simultaneous heavy jobs. The existing disk-pressure policy can further constrain heavy/I/O work. Admission is serialized across server processes and fails before spawning excess runners. Replaying a known operation remains possible when the admission pool is full.

Cancellation is `cancel_requested` while a verified runner or child is alive. A completed cancellation is reported only after the host processes are observed stopped. Job status exposes fresh child PIDs and flags stale heartbeats even when the runner remains alive. A runner refuses to resurrect a job already marked terminal.

Legacy Docker cancellation stays pending while the local runner or child remains alive. Once those local processes stop, the job becomes terminal `interrupted`, with `workloadTerminationVerified: false` and an explicit explanation. This releases local runner admission without claiming the workload inside a shared container stopped. Durable host submission does not offer a Docker workload-termination guarantee.

## Authentication, deadlines and connector refresh

Cloudflare JWKS work runs outside the shared OAuth token/state lock. Fetching has a total ten-second deadline and a 256 KiB limit, including streamed responses. Refreshes are single-flight and briefly rate-limited; existing-token verification and registration remain responsive during a stalled refresh. Client registration is revalidated before committing a code after network verification.

Launcher health requests bound both headers and body reads by the remaining startup deadline and reject oversized/unexpected health bodies. Rust capture is tested through repeated new server instances. Legacy JavaScript capture assertions include the actual bounded tool diagnostic instead of only an opaque boolean.

Contract version 2 advertises tool-list change support. `devbox_status` includes the current native capability manifest so even a client with an older registered tool set can detect drift. Compare the actual client registration with `devbox_capabilities` and its schema hash. Client-side registries that persist imported schemas must refresh their connection/tool catalog; restarting the server alone cannot rewrite an external registry. Verify all 45 names and `io-heavy` after refresh.

## Validation

`agent-reliability-smoke.mjs` validates tool discovery, file conflicts/retries, duplicate submission, retention-safe receipts, admission limits, pagination, task revisions, restart recovery, verified cancellation and repeated native Windows capture. It runs in isolated directories and ports. Legacy 37-tool schemas/results remain checked separately; the generated native-name manifest adds eight explicitly tested tools rather than hiding drift.

```sh
cargo fmt --manifest-path rust-mcp/Cargo.toml -- --check
cargo clippy --manifest-path rust-mcp/Cargo.toml --locked --all-targets -- -D warnings
cargo test --manifest-path rust-mcp/Cargo.toml --locked
cargo build --manifest-path rust-mcp/Cargo.toml --locked
node rust-mcp/scripts/agent-reliability-smoke.mjs
node rust-mcp/scripts/check-contract-parity.mjs
node scripts/check-runtime-contract.mjs
```
