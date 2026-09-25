# Production Devbox harness audit and implementation report

Date: 2026-09-25. Audited production and repository base: `43127402af2b82cd00561017570f350335593057`.

## Executive assessment

The production service already has a native model/tool harness with durable runs, independent drivers, scoped operator grants, bounded model/tool budgets, uncertainty handling and a SQLite coordinator. Replacing it with another framework is not the highest-value first step. The immediate gaps are correctness at state-transition and provider-stream boundaries.

This review produced a focused C++ candidate, an independently executable regression suite, and a [technical refinement draft](HARNESS_REFINEMENT_DESIGN_2026-09-25.md). The candidate changes three runtime source files. It does not widen autonomous execution authority or introduce a new paid service. The lead team should verify, amend or discard the changes before any promotion.

The new negative-control suite was run against the unmodified production runtime source: **15 of 18 named cases failed; three control cases passed**. The existing `runs` and `providers` suites both passed on that base. These results distinguish newly reproduced gaps from an already-broken checkout. They are functional regression evidence, not an agent benchmark score.

## Scope and production observations

The live `devbox_capabilities` and `devbox_status` responses were inspected directly during this task. The production checkout and `origin/main` initially agreed with the serving revision. Development, builds and fixture outputs used an isolated worktree and the branch `audit/harness-reliability-20260925`.

| Observed field | Reported value |
| --- | --- |
| Serving implementation | Native C++, Windows host mode |
| Contract / tool count | 9 / 53 |
| State backend / schema / coordinator protocol | SQLite / 2 / 1 |
| Source revision | `43127402af2b82cd00561017570f350335593057` |
| Serving binary SHA-256 | `58cb55e051e8000c8da5af9a54498cbe288cf9b8a70c51a285d38b3ab9561129` |
| Tool schema SHA-256 | `685181742bdd4a5e2b7136da45feef6582fc5f75a110f61a622ae4df1d0c5366` |
| Source dirty flag | false |
| Backend runs / resumable artifact uploads | Reported supported |
| Guardian / public tunnel | Healthy in the sampled responses; four tunnel connections, zero reported request errors |
| Execution capacity | Six weighted execution slots; one reserved interactive slot; separate watch capacity of four |
| Memory/GPU/disk reservation dimensions | All zero: aggregate limits for those declared dimensions are disabled, not evidence of enforced OS limits |
| Job-store accounting at initial sample | Approximately 9.0 MB, 269 retained terminal jobs, no reported quota pressure |
| Initial process RSS | 17,383,424 bytes; a point-in-time observation, not a load-test result |
| Initial five-minute event-loop p95 / p99 | Approximately 14.76 / 15.33 ms; not end-to-end tool latency |
| Disk condition | Warning at 4.29% free, with approximately 320 GiB still free; no unrelated cleanup was performed |

The existing host tools execute with trusted-operator host authority. Their read-only hints are advisory, not OS confinement. Autonomous program execution is a separate grant-bound LPAC path on this Windows host; the advertised profile denies network, desktop input and child processes. The patch does not blur that distinction.

Two ordinary execution inspections encountered approximately 15-second admission timeouts. The first baseline build recorded a 39,617 ms admission wait; a later baseline rebuild recorded 27,779 ms. Subsequent snapshots were not simultaneous measurements of the failed admissions, so they do **not** establish the reason or prove scheduler starvation. Status and durable-job inspection remained available. The design draft proposes the additional diagnostics needed to resolve this observation rather than changing production fairness settings speculatively.

No production binary, service, provider configuration, execution policy or persistent deployment setting was changed. Neither README was edited. Temporary, uniquely named Windows qualification tasks were used only to provide an independent test frontend and are removed after their receipts are collected. They do not schedule recurring work.

## Reproduced findings and candidate fixes

### H1 — Terminal state can be resurrected by a stale transition

**Priority: high.** In [`RunController::save`](../cpp-mcp/src/runs.cpp), an approval or reconciliation can read a nonterminal run, then observe an operator cancellation at the save boundary. The previous implementation copied the newer revision/control into the older proposed record and persisted that older ready state. Terminal cancellation was therefore not an immutable result.

The fix returns the already-committed terminal record without a new mutation or event. Model and tool admission sites also stop before invoking callbacks when that terminal transition wins; preserving the status alone would not be sufficient to prevent work from being dispatched. The regression suite injects exact read interleavings through a wrapper that delegates storage to the actual SQLite implementation. It does not rely on probabilistic sleeps.

Covered cases: approval/cancellation, reconciliation/cancellation, model admission/cancellation and granted-tool admission/cancellation. The latter cases check both the final revision and that no new provider/effect callback ran after losing admission.

### H2 — A losing or late driver can publish a false failure

**Priority: high.** [`RunService::drive`](../cpp-mcp/src/run_service.cpp) previously caught exceptions after `drive_impl` had released its ownership lock. Failure to acquire the driver lease could therefore overwrite the actual owner's run. An already-completed run could also become failed if a late invocation encountered missing provider configuration before checking terminal state.

The fix holds the driver lease across execution and failure publication, checks terminal state before provider configuration, and protects the failure write with a controller lease and revision compare-and-swap. Startup and ownership failures cannot publish a competing driver's failure.

Covered cases: completed, cancelled and failed records remain byte-for-byte equivalent in their public state and revision; a competing real file-lock owner is not disturbed; no false failure event is appended. Two positive controls ensure that a real owning-driver configuration failure is still recorded, and an admitted unknown outcome remains uncertain rather than becoming safe to retry.

### H3 — Pause can weaken an accepted cancellation

**Priority: high.** An in-flight run could accept cancellation and then have a delayed pause request replace its control intent, leaving a resumable state.

The fix makes cancellation take precedence over a later pause. The caller receives the current cancellation acknowledgement without changing revision or appending a pause event. Cancellation acknowledgement is still distinct from proof that an external provider has stopped or that billing is known. The test checks that uncertainty remains visible and a subsequent step cannot regenerate the cancelled run.

### H4 — Streamed function-call order is sorted as text

**Priority: medium.** [`ProviderStream`](../cpp-mcp/src/provider_protocol.cpp) used one string-keyed map for opaque Responses item IDs and decimal Chat Completions indexes. Sequential assembly consequently placed index 10 before index 2.

The fix separates the numeric index map from the opaque-ID map. Tests assemble one, twelve and thirty-two calls with reverse arrival order and byte-at-a-time fragmentation, then verify numeric call order. This preserves the provider's indexing; it does not assume that independently proposed parallel calls imply a semantic dependency order.

### H5 — A rejected stream can retain an earlier executable batch

**Priority: medium, defense in depth.** A consumer that caught `feed` failure and then called `finish` could receive previously completed calls from the same stream.

The fix permanently invalidates the unfinished stream on a feed exception, clears executable data before allocating diagnostic strings, and makes later reads non-executable. Responses and Chat Completions both have negative controls. A successfully finalized result remains stable when additional input is rejected.

The production transport already discards its failed exchange, and grants remain mandatory. This finding is not a demonstrated remote execution or grant bypass; the change closes a dangerous parser-API recovery behavior for present and future consumers.

### H6 — Streaming validation is weaker than non-streaming validation

**Priority: medium.** Chat deltas did not consistently reject an alternate output role, unsupported tool discriminator or missing call index.

The fix validates these fields before assembly. It deliberately preserves omitted or null incremental metadata, as documented for later fragments, and accepts usage-only terminal chunks. A positive compatibility test verifies fragmented arguments with null role/type/name/ID placeholders. No new schemas or tools are introduced.

## Verification and evidence

The test-only baseline commit is `d474412`; it contains the new CTest target and regression suite while leaving the runtime source identical to the audited base. The initial legacy suites passed, while the final negative-control run reported 3 passed / 15 failed across 18 named scenarios. Those failures are the expected baseline result, not an unreported qualification failure.

The first attempt to run driver tests under a generic managed execution job correctly refused independent durable children. Those six launch errors were **not** counted as reproduced driver bugs. The suite was rerun under an explicitly owned, independent Windows qualification frontend; it then reproduced all four driver-integrity failures and passed both driver error controls. No production process-ownership check was relaxed to make the tests pass.

### Candidate qualification result

All eight local qualification steps completed successfully at `2026-09-25T11:14:16.9838597Z`. Tested runtime-source commit: `f0c5dd10899cb6d9e239e589656096992aef2c64`. Exact candidate executable SHA-256: `e3268d80ccdf612b65538e97738bd4b0fb10a74930627c97938673180ed32b91`. Any later commit in this PR is documentation/evidence-only unless explicitly identified otherwise.

| Check | Observed result |
| --- | --- |
| New regression scenarios against unchanged baseline | 3 passing controls / 15 expected failures |
| Same scenarios against candidate | 18 passed / 0 failed |
| Selected native CTest suites | 35 passed / 0 failed; 4 of 39 suites explicitly excluded |
| Repeated harness / run / provider suites | 20 successful invocations each; 60 total suite invocations |
| C++ runtime contract check | Passed |
| Shared compatibility schema parity | 37 shared tools; zero input/output/metadata differences and zero validity errors |
| Full native SDK contract | 53 tools; passed |
| Engine and agent-reliability SDK flows | Passed, including durable retry, byte round-trip, admission/drain and cancellation |
| Backend model/tool harness with Tasks extension | Passed: two local recorded model requests, one actual isolated effect, frontend restart, exact operator grant and completed receipt replay |
| Paid provider requests | Zero |
| Production and README preservation | Live serving identity unchanged; both root README Git blobs unchanged |

The [machine-readable validation manifest](audit-evidence/2026-09-25-harness/validation.json) binds source/binary identities, exact commands, exit codes, exclusions, scenario verdicts, repeat counts and SHA-256 hashes of the published logs. [Baseline verdicts](audit-evidence/2026-09-25-harness/baseline-final.txt), [candidate verdicts](audit-evidence/2026-09-25-harness/candidate-regressions.txt), [native CTest output](audit-evidence/2026-09-25-harness/candidate-native.txt), [repeat output](audit-evidence/2026-09-25-harness/candidate-repeat.txt), and [backend SDK output](audit-evidence/2026-09-25-harness/backend-harness-sdk.txt) are retained. Only absolute user/worktree paths and line endings have been normalized; test verdicts are not edited. The [production observation record](audit-evidence/2026-09-25-harness/production-observation.json) explicitly identifies its selected fields as transcribed observations, not a signed raw response.

The existing agent-reliability SDK test also performs read-only display capture across three fixture restarts. It retains only pass/fail metadata in the published evidence; no captured images are included. The four excluded standalone native suites were not run. A legacy test label refers to “Rust server starts”, but the fixture actually used the candidate C++ executable via `DEVBOX_MCP_TEST_BINARY`; the independent engine and backend SDK records bind the same candidate hash.


Reproduction on an independently owned developer/CI frontend:

```text
cmake --build <build-directory> --config Release --parallel 3
ctest --test-dir <build-directory> -C Release -R "^(harness-reliability|runs|providers)$" --output-on-failure
ctest --test-dir <build-directory> -C Release -R "^(harness-reliability|runs|providers)$" --repeat until-fail:20 --output-on-failure
```

For the negative control, build `d474412` in a separate checkout with the same toolchain and run `harness-reliability`; its expected exit code is nonzero. Run the candidate in a separate build directory. An incremental build duration must not be compared with a clean baseline duration as a runtime-performance result.

For the native SDK checks, set `DEVBOX_CPP_BINARY` and `DEVBOX_MCP_TEST_BINARY` to the exact candidate executable, and `DEVBOX_ISOLATION_PROBE` to the candidate probe. The existing scripts allocate their own loopback ports, state roots and fixture workspaces. `DEVBOX_TEST_TASKS=1` enables the negotiated Tasks-extension branch of the backend harness smoke test. Never point these fixtures at production state.

## Qualification limits and required lead review

The local configuration is Windows MSVC Release with C++23, TUI disabled and link-time optimization disabled. It reuses existing native dependency installations selected through explicit CMake paths. This is not a fresh dependency rebuild, a signed release, or a substitute for hosted sanitizer and multi-platform qualification.

The four named standalone native desktop-control, capture and installer suites are intentionally excluded from execution to avoid disturbing the user's desktop or mixing deployment behavior into the harness audit. Their targets are built. The existing hosted CI remains responsible for the complete applicable matrix, including Linux, macOS, ARM64, Android, sanitizers, packaging, promotion and installation checks. Any queued or pending PR check must remain reported as pending rather than inferred from local passes.

No live commercial model request was made. The provider and SDK fixtures use recorded/local responses, with actual scoped program execution in the backend smoke test. No agent-quality benchmark, percentage score gain, production load benchmark or cross-platform performance improvement is claimed.

The lead team should prioritize the follow-on transition-table audit, typed argument validation, bounded run-artifact retrieval for compacted contexts, explicit capacity policy, queue diagnostics and independently verified task-quality evaluation described in the [technical draft](HARNESS_REFINEMENT_DESIGN_2026-09-25.md). These are proposals, not capabilities silently claimed as implemented in this patch.
