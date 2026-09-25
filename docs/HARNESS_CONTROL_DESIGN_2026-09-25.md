# Devbox harness control design and next-stage proposals

**Date:** 25 September 2026

**Status:** Sections 1â€“4 describe the implemented candidate. Section 5 contains proposals only. Release qualification remains subject to lead review.

## 1. Design objective

Strengthen the durable harness without replacing its existing execution architecture or widening model authority. A run must preserve operator intent, distinguish observed results from unknown external outcomes, and account for admitted work without charging a callback that this invocation can prove it never entered.

The existing provider adapter, scoped grant authority, isolated program broker, SQLite store, bounded context artifacts, and durable receipts remain the foundation. A tool annotation is not an OS sandbox, and an accepted cancellation is not proof that an external effect was undone. Those boundaries are preserved rather than hidden behind a new abstraction.

## 2. Control and settlement contract

The `status` field describes what the run may do next; `phase` records the durable workflow point. A paused approval wait is not a runnable model step. A pending phase after interruption is not evidence that nothing was dispatched.

| Observation | Resulting behavior | Accounting / evidence |
|---|---|---|
| Pause while waiting for approval | `paused`, with the exact approval request retained | No grant or dispatch is created by pause |
| Resume that paused approval wait | `awaiting_approval`, not `ready` | No driver launch; same operation identity remains approvable |
| Fresh admission followed by observed pause before callback entry | Restore the ready phase and settle `paused` | Refund this fresh reservation; resume may dispatch the original step once |
| Fresh admission followed by observed cancellation or expiry before callback entry | `cancelled`, or `failed` for expiry | Refund counters/reservation because this invocation proves non-dispatch |
| Recovered pending work followed by pause | `uncertain` | Mark unknown outcome; retain reservation; ordinary resume is rejected |
| Recovered pending work followed by cancellation or expiry | Terminal local state, with unknown outcome still explicit | Retain reservation; do not claim remote cancellation or rollback |
| Final model reply racing an already accepted cancellation | `cancelled`, with durable result/accounting retained | Do not discard the evidence or report task success merely because a reply arrived |
| Pause during a model step that produces its final reply | That step may finish as `completed` | Pause prevents further dispatch; it is not retroactive cancellation |
| Cancellation after an already committed terminal result | No rewriting of that terminal result | Preserve its revision and evidence |

The implementation does not close the physical interval between the final observation and an external call. Closing that interval globally would require the external system to participate in a cancellation/idempotency protocol; a local database transaction alone cannot provide it.

### Fresh versus recovered admission

`stop_before_dispatch` is a private helper used only immediately after a new admission within the same invocation. It must not be used to refund a recovered `model_pending` or `tool_pending` record. A persisted pending marker does not reveal whether a network request, subprocess, or side effect occurred before an interruption.

Model suppression restores the fresh round, token, and monetary reservations. Tool suppression restores its call and repetition accounting without consuming the scoped operation grant. Refunds are persisted through the same state transaction mechanism. A failure to persist that refund is not represented as a successful refund; recovery remains conservative.

## 3. Concurrency, persistence, and authority

The controller file lease serializes steps, approvals, and operator reconciliation. Cancellation and pause use short state updates rather than waiting behind a potentially slow model or program callback.

Each state commit checks the latest revision and merges the latest control. Cancellation cannot be downgraded by a later pause. A terminal record that another transaction has already committed is not overwritten by a stale controller snapshot.

A revision conflict retries the local state commit, not the external action. The retry budget is eight attempts. Each attempt starts from the original proposed status before applying the newly observed control. This matters when a tentative pause loses to a newer resume: retaining the previous attempt's normalized status would strand the run in `paused` even though its latest control was cleared.

Events and state are written together. Terminal metadata reflects the committed status rather than always calling an acknowledged control nonterminal. Non-revision storage failures are not silently retried as if the operation were known to be safe.

Authority remains unchanged: the model can propose an operation, not authorize one. Approval binds the original principal, run, operation, tool, and arguments. Repeated observation, pause/resume, state-commit retries, and frontend restart do not issue a new grant or broaden an existing grant.

## 4. Verification and compatibility

The native suite combines state-machine checks, deterministic callback interruption, and injected transaction-boundary interleavings backed by real SQLite. Positive controls retain existing completed-effect receipt replay, immutable completed results, ordinary pause-at-final behavior, and rejection of approval while paused. Nonzero synthetic monetary reservations verify actual refund/retention arithmetic without paid requests.

The MCP smoke fixture exercises a real temporary frontend, local mock provider, operator-issued grant, restricted native worker, frontend restart, and durable receipt replay. It runs with and without the Tasks extension. The restored approval is exercised, not merely compared in memory.

The existing tool registry, contract version, and state schema are unchanged. Clients must handle the corrected `awaiting_approval` result after resume and must not assume that every replay/resume response includes a newly started driver. Events gain truthful metadata; no client should use a false terminal flag as a substitute for actual state.

For ordinary verification in a configured checkout:

```text
cmake --build <build> --config Release --target devbox-runs-control-tests devbox-runs-tests devbox-provider-tests
ctest --test-dir <build> -C Release -R "^(runs|runs-controls|providers)$" --output-on-failure
ctest --test-dir <build> -C Release -R "^runs-controls$" --repeat until-fail:20 --output-on-failure
```

Run the repository's standard native CI and SDK workflow for release qualification. For the SDK smoke, select the built `DEVBOX_CPP_BINARY` and `DEVBOX_ISOLATION_PROBE`; run `node cpp-mcp/scripts/run-harness-smoke.mjs`, then repeat with `DEVBOX_TEST_TASKS=1`. All state and model traffic must remain confined to the fixture.

To reproduce the baseline comparison, apply only the new native test file and CMake test registration to commit `43127402af2b82cd00561017570f350335593057`. Do not apply the controller implementation. Expected result for this retained corpus: 4 pass, 22 fail. This is an intentional red baseline, not a reason to weaken the assertions.

## 5. Proposals for lead-team decisions â€” not implemented

These proposals are separate from the reviewed control patch. They should not be described as capabilities delivered by this PR.

### P1. Fence driver failure settlement and takeover

**Priority:** high. Source inspection identifies an adjacent area for adversarial qualification: the outer driver failure handler can write run failure state, while ownership is managed by driver/launch locks and process-instance records. This patch does not redesign that lifecycle.

Introduce a durable driver generation or fencing token, and require it for driver-owned failure settlement. A contender that never acquired the lease must not mark the active owner's run failed. Keep failure settlement inside the ownership boundary, and prevent late error handlers from rewriting terminal state. Do not rely on a PID without its process-instance identity.

**Acceptance evidence:** two competing drivers, owner termination at each boundary, stale callback after takeover, failure while retiring, and already-terminal records. The losing contender must cause no state mutation and no duplicate dispatch. Verify the actual failure path before choosing the smallest implementation; this is not a claim that every listed interleaving was reproduced in this audit.

### P2. Reclaim unattended waiting capacity without erasing uncertainty

**Priority:** high. A driver intentionally exits for an approval wait or pause. Expiry/accounting for unattended nonterminal runs therefore needs its own explicit lifecycle policy rather than dependence on another model step.

Design a paged, resumable expiry sweep over indexed states. Retire expired known-idle waits while retaining audit history and operation identities. Unknown pending outcomes need a distinct operator-visible reconciliation policy; expiry must never authorize a retry or turn an unknown effect into a claim of non-execution.

**Acceptance evidence:** expiry with no polling client, restart during a sweep, interrupted pending work, stable cursor bounds, tenant isolation, and admission capacity recovery. Measure sweep duration and reclaimed waiting capacity; do not assume deletion of run history is safe.

### P3. Add externally checked harness task evaluations

**Priority:** high for any claim about general task score. The delivered regression suite measures the selected control defects, not general task success.

Build a versioned corpus with exact deterministic oracles: repository change and test outcomes, integer arithmetic with Unicode-preserving file round trips, denied unauthorized effects, stale-versus-current evidence labeling, and interruption/recovery. Keep the same prompts, fixtures, budgets, and seed sets for baseline/candidate comparisons. Separate task completion, evidence fidelity, unauthorized-effect rate, unknown-outcome reporting, latency, and cost.

**Acceptance evidence:** an independent checker, retained inputs and receipts, reproducible reruns, no self-awarded success, and zero unauthorized effects as a release gate. Report uncertainty and partial coverage. Only then claim a broader score improvement or compare different models/providers.

### P4. Qualify aggregate resource admission under realistic contention

**Priority:** medium. The inspected production configuration had zero aggregate memory/GPU/disk capacities. The runtime explicitly says that zero disables those dimensions. It does not mean memory usage is zero or that inferred reservations enforce OS limits.

Define operator-reviewed capacities and workload reservations separately from actual RSS/VRAM/disk telemetry. Preserve interactive capacity under concurrent local inference, builds, file inspection, and research. Stage changes behind a canary rather than guessing a memory or GPU budget from a device name.

**Acceptance evidence:** no admitted-reservation oversubscription, bounded cancellation/release time, correct orphan reconciliation, and measured throughput and p95 queue delay under a pinned workload. Performance gains remain unquantified until measured.

### P5. Extend context retrieval with bounded, attributable evidence

**Priority:** medium. Reuse the existing hashed artifacts and role boundaries before adding another memory store.

Prototype bounded artifact-range retrieval and relevance selection with explicit source hashes, truncation indicators, and immutable references. Model-provided text must never become a new system instruction, grant, or executable authority. Evaluate compaction against long tasks with known required facts and adversarial source text.

**Acceptance evidence:** answerability after compaction, stable source attribution, bounded context and artifact bytes, preservation of the original goal, and no authority escalation through retrieved content. Compare measured task outcomes and token budgets, not just shorter prompts.

### P6. Make connector/runtime drift diagnosable

**Priority:** medium. Compare the registered client-action set with the live contract version, schema hash, and per-tool schema. Clearly distinguish a backend-supported feature, an exposed connector action, a granted scope, and an operationally qualified capability.

Prefer an explicit compatibility warning and authoritative per-tool discovery to silent fallback, repeated rediscovery, or assumptions that cached schemas are current. Keep administrative connection/permission changes operator-controlled.

**Acceptance evidence:** stale schema fixtures, intentionally restricted connectors, missing scopes, unsupported runtime profiles, and reconnect recovery. Diagnostics must not expose credentials or mislabel a client restriction as a server outage.

## 6. Release and rollback gates

The lead team should review the exact implementation and evidence, run existing cross-platform and sanitizer CI on the final head, and complete the three desktop-dependent gates in an authorized interactive session. Validate a pinned, clean release build with the production optimization settings; the local development build intentionally disabled interprocedural optimization and TUI.

Canary with isolated state and mock providers first. Then verify approved real workloads under explicitly bounded provider budgets. Observe cancellation settlement, unknown-outcome incidence, approval-wait behavior, state-conflict exhaustion, and duplicate-effect prevention. Do not automatically enable new aggregate resource settings as part of this control patch.

No state migration is introduced. Rollback is a reviewed code/build rollback, not deletion of grants, receipts, or unknown-outcome evidence. Restoring the old build also restores its old control behavior; operators must not assume that reverting code resolves an outstanding external effect.

## Primary references and implementation evidence

- [Production-backed audit and measured results](HARNESS_CONTROL_AUDIT_2026-09-25.md).
- [Controller implementation](../cpp-mcp/src/runs.cpp), [service lifecycle](../cpp-mcp/src/run_service.cpp), and [regression corpus](../cpp-mcp/tests/runs_control_tests.cpp).
- [MCP tool security considerations](https://modelcontextprotocol.io/specification/2025-11-25/server/tools): user control, consent, validation, and the limited trust warranted by tool annotations.
- [SQLite isolation documentation](https://sqlite.org/isolation.html): database transaction isolation is not a transaction protocol for arbitrary remote side effects.
