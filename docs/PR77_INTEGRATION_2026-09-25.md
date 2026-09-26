# PR77 integration: admission, controls and qualification

This follow-up integrates PR77 onto `main` at `11c3fd17eec4ea344a1cae61ecf2027e77ba30c4`, retaining PR76's provider-stream and driver-ownership fixes. It addresses lead review `5319680713` rather than replacing either side's controller wholesale.

## Admission ownership

`RunController::save` returns whether this invocation committed the transition. Every retry re-reads the authoritative state. A terminal state, or a pause/cancel/uncertain state that wins before model/tool admission, returns the existing record with `false`: no new revision, admission event, callback or reservation. Admission callers immediately return on `false`.

Only a newly committed admission can reach `stop_before_dispatch`. That second boundary observes a later control, cancellation token, expiry or terminal result before entering the external callback. Known non-dispatch refunds only the reservation created by that invocation. Defensive arithmetic checks reject impossible accounting rather than underflowing. A terminal observation is preserved without rewriting its evidence.

Recovered pending work never takes the fresh-refund path. A storage failure while persisting a refund does not turn the durable pending marker into a claim of non-execution; recovery retains the reservation and requires reconciliation.

Approval and reconciliation retain PR77's controller lease. A terminal transition discovered after taking that lease is returned unchanged, preserving PR76's cancellation-race behavior. Approval-wait resume restores `awaiting_approval`, and create replay/control actions do not manufacture drivers for dormant results.

## Regression coverage

The original 26 PR77 cases are retained. Two cross-review cases require PR76's pre-admission no-op. Eight additional cases cover model/tool pause and cancellation winning the admission compare-and-swap, terminal publication before dispatch, and refund-persistence failure. The combined controller corpus is **36 scenarios**.

PR76's **43-scenario** harness suite remains separately registered and unchanged. Its semantic requirements must not be weakened if a future implementation changes the number of state reads. Both the native controller and service/SDK tests are release gates.

Historical measurements in `docs/evidence/harness-control-20260925/` describe the original PR77 branch on `43127402...`, not this integrated head. Lead review independently found 9/26 passing on PR76's controller and 26/26 on original PR77; adding the two no-op cases exposed the latter's regression. The standalone integrated 36-case probe passed before the full qualification; final-head native/SDK/hosted results are recorded in the PR discussion with exact source and executable identities.

## Qualification and scope

No public tool, grant authority, state schema, provider configuration, dependency version, production capacity or deployment setting changes here. Local development builds reuse installed dependencies, with TUI and interprocedural optimization disabled; they are not signed shipping artifacts. Standalone local desktop/input/setup tests may be excluded during workstation activity only when explicitly recorded, not counted as passes. Hosted final-head qualification remains mandatory.

Performance experiments are a separate follow-on branch. They must compare matched fixtures, preserve full durability and per-call state freshness, and distinguish native microsecond component times from end-to-end transport/process latency. No benchmark or broad task-score gain follows from the controller scenario count.

The design document's P1 driver-ownership observations predate PR76: retain PR76's delivered locks, launch checks and failure-publication guards. Only residual independently reproduced gaps justify additional redesign. P2-P6 remain separately scoped, unimplemented proposals, not delivered capabilities or prerequisites for these correctness fixes.

No production upgrade is authorized by this implementation record. Any later rollout requires the final signed tested artifact, isolated upgrade/rollback canary, drain, independent recovery helper and actual connector reconnection acknowledgement.
