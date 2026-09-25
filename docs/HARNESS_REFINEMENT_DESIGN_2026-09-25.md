# Devbox harness refinement: technical draft for lead review

Date: 2026-09-25. Audited base: `43127402af2b82cd00561017570f350335593057`.

This draft separates the narrow candidate implementation from follow-on proposals. It does not authorize deployment, new providers, increased spending, or wider execution grants. The lead team may retain, revise or reject each proposal independently. See [the audit and evidence](HARNESS_AUDIT_2026-09-25.md) for actual qualification results rather than treating this design as proof.

## 1. Architecture and invariants

The existing native harness is already more than an execution-tool collection: `RunService` starts an independent driver, `RunController` persists model/tool transitions, `GrantAuthority` binds exact effects to operator authority, and the indexed state coordinator serializes state transactions. Provider adapters normalize two wire protocols, while the program broker confines autonomous execution to a qualified private workspace. Ordinary ChatGPT-invoked host tools remain a different, trusted-operator authority surface. A read-only hint is not a sandbox.

Keep these boundaries. Do not turn the broad host shell or grant-management CLI into model tools to improve a benchmark result. The useful improvement is reliable control and evidence at the boundaries already present.

The candidate enforces the following additional invariants:

1. A terminal run is not restored to an executable state by an older approval, reconciliation, or driver failure.
2. Only the holder of the run's driver lease can publish that driver's failure. Ownership persists until the failure transaction finishes.
3. Once cancellation has been accepted, a delayed pause cannot weaken it into a resumable state.
4. Chat Completions function calls retain numeric provider-index order. Arrival order and decimal string sorting do not define effect order.
5. Any exception while consuming an unfinished provider stream permanently prevents that stream from returning executable calls. Successful finalization remains stable.
6. Streaming and non-streaming adapters enforce the assistant-role/function-tool boundary consistently; required numeric indexes are not silently synthesized.

These invariants do not guarantee that a model's final answer is correct. `task_success: not_independently_verified` remains truthful and unchanged.

## 2. Candidate implementation

### 2.1 Terminal-state precedence

`RunController::save` already re-reads state to preserve control requests that arrive during a slow callback. Previously, it rebased the revision and copied control even if the newly observed record had become terminal. That permits an approval or reconciliation begun before cancellation to publish its earlier ready state afterward.

The candidate first checks the newly observed terminal status. When terminal, it replaces the caller's local record with that committed record and returns without a mutation or event. The public result therefore reports the real cancellation, including its revision, rather than reporting an approval that no longer has effect.

Both model and tool admission sites then inspect the returned terminal record and return before invoking the provider or effect callback. Preventing a state write alone would not prevent a cancelled operation from being dispatched.

This is deliberately smaller than a rewrite of the state coordinator. Ordinary compare-and-swap still rejects a concurrent write that happens after the read. It does not silently retry arbitrary transitions. File artifacts written before a losing transition can remain unreferenced; this change does not introduce unsafe garbage collection to remove them.

### 2.2 Driver ownership includes the exception path

The driver lease moves to the outer `RunService::drive` scope. A nested exception handler may publish `driver_failed` only while that lease remains held. Failure to acquire ownership exits without modifying shared run state. The failure handler also takes the controller lease, re-reads the record, protects terminal states, and uses the existing revision compare-and-swap transaction for the state and event.

A completed, cancelled or failed run is recognized before reading provider configuration. This means a late driver invocation cannot reinterpret a durable answer because an operator subsequently removed or changed a provider profile.

Startup, lock acquisition, or racing compare-and-swap failures still return a nonzero driver result. They do not acquire permission to rewrite another actor's decision. A real owning-driver configuration failure remains a failure; a failure after an admitted but unconfirmed model/effect remains uncertain. No automatic retry of an unknown external operation is added.

### 2.3 Cancellation precedence

A pause request that observes `control: cancel` returns the current cancellation acknowledgement without appending a pause transition or changing revision. Other existing control semantics remain in place. In particular, cancellation acknowledgement is not proof of remote provider termination or settled billing. A completed model response racing with cancellation is not assigned new semantics by this patch; the existing completion/cancellation policy should be specified separately rather than changed accidentally.

### 2.4 Provider stream integrity

Opaque Responses item IDs continue to use their existing string-keyed map. Chat Completions fragments use a separate integer-keyed map, bounded by the existing 32-call limit. Final assembly preserves numeric ordering even if fragments arrive in reverse or interleaved order.

The `feed` exception path marks the stream finalized with a protocol-error result and clears calls, native output, text and fragment buffers. A subsequent `finish` cannot recover an earlier executable batch. Already emitted unkeyed text deltas cannot be retracted; consumers must still treat them as provisional until successful finalization. The higher-level keyed transport retains its credential-echo barrier.

Chat fragments now validate the assistant role, the optional tool-type discriminator, and explicit integer indexes. Usage-only final chunks remain supported. This is protocol hardening, not a claim that an alternate streamed role previously bypassed grants: the controller already constructs assistant context and the production transport already discards stream failures.

## 3. Compatibility and rollout

No tool is added or removed. The MCP registry, external schemas, contract version, state schema, grant format, operator defaults, provider configuration and production services remain unchanged. The legacy JavaScript and frozen Rust runtimes are not modified. Both README files are preserved.

The intended behavioral changes are strict rejection of malformed streams; stable numeric call ordering; terminal-state precedence over older transitions; and non-mutating loss of driver ownership. Clients that emit malformed Chat Completions chunks may now fail explicitly instead of relying on permissive parsing. Qualify each supported local server with recorded conforming and malformed examples before promotion.

Normal hosted CI discovers the additional CTest suite through `cpp-mcp/CMakeLists.txt`. A local Windows Release build with link-time optimization disabled is useful development evidence, but is not equivalent to the signed, production-configuration, multi-platform release gate. No live deployment is part of this PR.

Lead acceptance sequence:

- Review runtime changes independently from the evidence and proposed roadmap. Re-run the added tests against the audited base to confirm their negative controls.
- Run all applicable hosted native, sanitizer, transport, grant/isolation, state migration, and backend SDK tests. Compare the generated tool schemas to the base.
- Qualify the final commit using the repository's signed promotion and canary procedure; inspect active runs and preserve unknown-operation receipts.
- Deploy only after explicit approval. Retain the known-good signed binary and state backup. Because this patch changes no state schema, reverting the binary does not require a schema downgrade, but it also reintroduces the diagnosed defects. Do not delete operation receipts as part of rollback.

## 4. Follow-on proposals, not implemented here

### A. Complete state-transition concurrency specification

Priority: high. Define an explicit transition table for status, phase, control intent and revision. Audit every writer, not only the driver and save paths exercised here. Specify the winner for final-answer completion versus accepted cancellation; repeated approval; pause while awaiting approval; expiry while idle; and reconciliation overlapping a controller restart.

Use transactional expected-state predicates where a plain revision predicate does not express the policy clearly. Add deterministic two-actor interleavings and real process-exit tests. Never resolve an uncertain model/effect merely by observing that its local driver is gone. A state notification is an optimization; authoritative state and process identity remain the recovery basis.

Acceptance: no resurrection of terminal records; no duplicate effects; no change to durable uncertainty without an explicit, auditable resolution. Unknown effects must retain their receipts after retention and maintenance.

### B. Typed model-tool contracts and evidence access

Priority: high. Validate proposed arguments against an explicitly supported schema subset before exposing an approval request, then revalidate the exact granted arguments before dispatch. Reject unsupported schema constructs rather than silently treating them as validated. Keep the definition's schema hash in the run fingerprint.

A useful expansion is a bounded, read-only run-artifact tool that can retrieve a specific run-owned digest and range. Today compaction can retain an artifact reference without providing the model a convenient native way to recover that evidence. The new tool should derive principal/run ownership from execution context, accept no arbitrary host paths, verify hashes, cap bytes/nodes/depth, and label content untrusted. It should not need host-shell authority. A model's artifact access must not allow grant inspection, secret retrieval or cross-run enumeration.

Acceptance: prompt-injection fixtures cannot introduce system roles or authorization; cross-principal and cross-run reads fail; a compacted multi-step task can recover exact evidence and finish correctly within its budget. This proposal intentionally does not widen autonomous program or network permissions.

### C. Explicit budget and capacity policy

Priority: medium-high. Separate declared reservation limits, measured OS resource usage, per-run ceilings, and provider-reported billing. Production's observed zero memory/GPU/disk reservation dimensions disable aggregate admission limits; that is not proof of an OS resource limit. Operator configuration should state which dimensions are intentionally disabled.

Add per-principal concurrent-run and reserved-cost observability, expiry accounting for idle approval waits, and bounded accounting for all retained run artifacts. Preserve at-most-once operation identities when applying retention. An expired run with an unknown external effect is not a safe candidate for automatic replay or receipt eviction.

Model transport retries must distinguish requests known not to have been dispatched from outcomes that may have been billed. Do not add generic exponential retry around generation. A rate-limit delay is evidence, not permission to retry an uncertain request.

Acceptance: capacity and cost tests cover multiple drivers, crash/restart, long approval waits and integer overflow; no over-admission; reserved unknown cost is retained and explained.

### D. Actionable observability before performance refactoring

Priority: medium. Extend structured phase events with bounded, non-secret timings: admission wait, provider transport, parse/validation, grant wait, isolated effect, and durable publication. Export count/distribution summaries, not goals, credentials, raw tool outputs or provider bodies. Provide a stable end-to-end correlation identity across run, model round, operation receipt and artifact.

One synchronous shell inspection in this audit encountered a 15-second queue timeout, and the baseline build waited about 39.6 seconds for admission. The later status snapshot does not establish the cause of either wait. Add decisive queue-class, oldest-eligible age, selected admission constraint and snapshot-age diagnostics before claiming scheduler starvation or changing fairness.

Benchmark status/query latency, coordinator transaction throughput, memory, CPU, and tail latency under matched workloads. Isolate resource limits, compiler/optimization flags, logging, network and cache state. Optimize only a measured bottleneck. More tools or a larger code rewrite is not a performance result.

### E. Independent task-quality evaluation

Priority: high before claiming a better agent score. Maintain a small versioned corpus of executable tasks with independently checkable artifacts: constrained edits, data transforms, interrupted builds, conflicting checkpoints, stale approvals, Unicode handling, compacted evidence recovery, and refusal of injected authority. Keep deterministic provider replays separate from live-model evaluations.

For each case record the source commit, runtime binary/schema hashes, model/provider revision, sampling settings, seed where supported, tool schema, environment, task budget, observed artifacts and verifier result. A model declaring success is not a pass. Score normal and fault-injected runs separately; do not silently discard failed attempts or count a resumed run as a new independent success.

Proposed gates, not measured improvements in this PR:

| Metric | Proposed rule |
| --- | --- |
| Unauthorized or duplicate external effects | Zero tolerance |
| Terminal resurrection / cross-principal artifact access | Zero tolerance |
| Deterministic regression correctness | Every fixed invariant passes; baseline negative controls fail as expected |
| Live task success | Verified artifacts and explicit per-case verdicts, reported with sample size and uncertainty |
| Recovery | Report successful recovery, uncertain outcomes and operator-required intervention separately |
| Efficiency | Compare wall time, p50/p95 admission latency, tokens, reserved/known cost and peak memory on matched tasks |

Live model use and paid-provider spending require a separate explicit budget and operator configuration. No live-model score, percentage improvement, throughput gain or production certification is claimed by this design.

## 5. Primary references

The repository source and tests are the evidence for the implementation findings. External references inform protocol and transaction design; they are not substitutes for runtime tests.

- OpenAI, function calling and streaming call assembly: https://developers.openai.com/api/docs/guides/function-calling (consulted 2026-09-25).
- SQLite, transaction and concurrent-writer semantics: https://sqlite.org/lang_transaction.html (consulted 2026-09-25).
- MCP, cancellation intent and completion races: https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation (consulted 2026-09-25). This is the core cancellation guidance, not a replacement for the separately negotiated 2026 Tasks extension implemented by this repository.
