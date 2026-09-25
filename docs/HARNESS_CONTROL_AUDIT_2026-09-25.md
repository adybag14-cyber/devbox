# Devbox production-backed harness audit

**Date:** 25 September 2026

**Disposition:** implemented candidate for lead-team review; not merged or deployed.

**Scope:** live production identity/health and focused source, regression, and integration review of the durable model-run harness. This is not an exhaustive security certification of every Devbox subsystem.

## Executive finding

Devbox already contains a substantive bounded model harness: durable runs, operator-configured providers, exact operation grants, isolated program execution, usage reservations, context artifacts, and explicit reconciliation for uncertain outcomes. The useful next step was not another tool catalog or a speculative rewrite. It was to make existing operator controls dependable across approval waits, callback boundaries, and competing state writes.

This candidate repairs six related classes of control-state defects. An identical, newly authored 26-case native regression suite was compiled against the unchanged baseline runtime sources and then the candidate: **4/26 passed before; 26/26 passed after**. Thus 22 previously failing scenarios now pass. These are targeted regression cases, not 22 independent root causes and not an independently scored general agent benchmark.

The final candidate also passed 20 consecutive repetitions of that suite and both MCP harness integration modes. The complete local native run passed **36/39 suites**. The remaining three desktop-dependent suites failed in the unavailable interactive desktop; they have not been waived, skipped in CI, or represented as passing.

## Production observation

The production service was inspected through live Devbox plugin calls, not inferred from an earlier audit.

| Property | Observed value |
|---|---|
| Serving commit | `43127402af2b82cd00561017570f350335593057` |
| Implementation | C++ native runtime |
| Compiler identity | MSVC `19.51.36256.0` |
| Contract / tool count | `9` / `53` |
| State backend / schema | SQLite / `2` |
| Source state | Clean, `main` |
| Serving binary SHA-256 | `58cb55e051e8000c8da5af9a54498cbe288cf9b8a70c51a285d38b3ab9561129` |
| Contract schema SHA-256 | `685181742bdd4a5e2b7136da45feef6582fc5f75a110f61a622ae4df1d0c5366` |
| Readiness | Runtime, guardian, and public tunnel reported healthy |
| Operational warning | Disk pressure: approximately 4.29% free in the sampled status |
| Model execution authority | Explicit scoped operator grant; model cannot issue grants |
| Aggregate resource reservations | Memory/GPU/disk capacities were zero: those aggregate dimensions were disabled, not OS enforcement guarantees |

Production source, service settings, provider configuration, deployment, and authentication were not changed. Development used a separate checkout and branch, separate build outputs, and temporary test state. No paid provider was invoked, no grant was issued into production model-run state, and the desktop was not unlocked.

The connector exposed 28 registered actions in this session while the serving runtime advertised 53 tools. That is a client/backend availability observation, not evidence that the server's tool count is false. It deserves explicit compatibility diagnostics rather than an assumption that every advertised action is available to every client.

## Confirmed defects and implemented repairs

Severity below is engineering prioritization, not a formal vulnerability score. Baseline failures and candidate outcomes are retained in the evidence directory.

| ID | Priority | Baseline behavior | Candidate repair |
|---|---|---|---|
| H1 | High | Resuming a paused `approval_wait` made the run `ready`. The driver could repeatedly observe the unchanged approval phase until exhausting its 512-step guard. | Restore `awaiting_approval`; retain the exact pending operation and grant request. Resume and create replay do not launch a driver for an approval wait. |
| H2 | High | An accepted cancellation could lose to a final model reply, a throwing callback, or a later pause. | Cancellation is sticky and merged at settlement. A reply completed before cancellation is committed remains terminal; an already accepted cancellation is not rewritten as success. |
| H3 | High | After durable admission, a newly observed pause, cancellation, or deadline did not prevent callback dispatch. | Recheck immediately before the newly admitted callback. Suppress dispatch and refund only that invocation's demonstrably unused reservations. |
| H4 | High | Stopping recovered pending work could hide the unknown external outcome or turn it into an ordinary resumable pause. | Preserve `external_outcome_unknown`, retain reservations, and require reconciliation before resuming uncertain work. |
| H5 | High | A competing control write could produce a raw revision conflict instead of a correctly merged transition. | Bounded compare-and-swap retries incorporate current controls without repeating the external callback. Re-evaluate the original proposed status on every retry so an uncommitted pause does not defeat a newer resume. |
| H6 | Medium | An immediately terminal cancellation event still reported `terminal:false`. | Persist event terminal metadata consistent with the acknowledged state. |

The dispatch refund tests use a **nonzero synthetic monetary reservation**, in addition to token, round, and tool counters. They do not depend on a zero-cost arithmetic identity. No real money is spent.

## Implementation boundary

The substantive implementation is in [`runs.cpp`](../cpp-mcp/src/runs.cpp), with a private helper declared in [`runs.hpp`](../cpp-mcp/include/devbox/runs.hpp). [`run_service.cpp`](../cpp-mcp/src/run_service.cpp) avoids unnecessary driver launches for waiting/paused/reconciled states that are not ready to execute.

Approvals and reconciliation acquire the existing controller lease, while pause/cancel requests remain independent of slow callbacks. State commit retries are capped at eight. Only revision conflicts are retried; other storage errors propagate. Neither the provider callback nor the tool callback is retried by these state-commit loops.

The new native suite is [`runs_control_tests.cpp`](../cpp-mcp/tests/runs_control_tests.cpp), registered as `runs-controls` in the existing CMake test graph. The existing SDK smoke test now checks pause persistence across frontend restart, exact approval identity after resume, absence of needless drivers, and subsequent execution under the original scoped grant.

There is no tool-registry change, dependency upgrade, provider credential change, state-schema migration, production capacity adjustment, or automatic rollout in this candidate. New event metadata is additive. The changed control semantics are intentional and documented in the accompanying design and backend-run guide.

## Validation evidence

The machine-readable records are under [`evidence/harness-control-20260925/`](evidence/harness-control-20260925/). The manifest identifies the tested source files, shared test-source hash, local build settings, binary hash, and actual recorded validation times.

| Qualification | Result |
|---|---|
| Unchanged baseline runtime + final new test source | 4 passed, 22 failed |
| Candidate runtime + identical test source | 26 passed, 0 failed |
| Repetition qualification | 20 consecutive successful runs of the 26-case suite |
| Existing run/provider baseline suites | Both passed before implementation |
| Full final native CTest run | 36 passed, 3 failed, 39 total |
| MCP SDK harness | Passed; 2 mock model requests, 1 isolated effect, frontend restart, exact operator grant, receipt replay |
| MCP Tasks extension variant | Passed with the same counts and checks |
| Paid requests | 0 in both integration modes |
| Whitespace and script syntax | `git diff --check` and Node syntax check passed |

The five additional persistence-boundary cases exercise final-reply cancellation during commit, sticky cancellation under a competing pause, pause/resume during approval commit, a bounded eight-conflict failure, and cancellation winning admission itself. Competing controls are committed to real SQLite through a test-only transaction-boundary wrapper. This is deterministic interleaving coverage, not a proof of every thread/process schedule.

### Desktop qualification remains open

The full test run, without exclusions, reported:

- `computer-native`: `COMPUTER_DESKTOP_UNAVAILABLE: unlock the interactive desktop first`.
- `computer-broker-native`: the owned broker did not become ready.
- `capture`: the test process had no capturable non-minimized, non-cloaked top-level window.

These are retained as failures. The unavailable desktop explains why local desktop qualification is incomplete; it does not justify weakening the existing gates. Lead review should rerun them in an authorized, unlocked interactive test session and investigate any persistent failure.

The local build used MSVC `19.51.36257.0`, x64 Release, bounded compiler parallelism, TUI disabled, and interprocedural optimization disabled. Existing dependency and Node installations were reused as inputs; they were not upgraded. Full-build warnings were observed in unchanged shutdown/filesystem-worker tests for integer narrowing. This is development qualification, not a fresh pinned-dependency, LTO-enabled release certification.

## What the results do not establish

Cancellation remains cooperative. A control accepted after the final pre-dispatch observation may race a real external request. Cancellation does not prove remote cancellation, undo a completed effect, or guarantee exactly-once execution across an arbitrary external service. Lost acknowledgements remain uncertain unless a durable receipt or explicit reconciliation resolves them.

No general task-success score, model-quality gain, inference-speed improvement, or end-to-end performance percentage is claimed. The 26-case corpus was selected to exercise the identified defects. The broader evaluation and capacity proposals in the design document require separate measurement.

## Lead-team handoff

Review the controller's control precedence, CAS retry semantics, reservation refunds, approval lease ordering, and unknown-outcome handling against the tests. Verify the final PR head in the existing cross-platform CI matrix, including sanitizer and release configurations. Complete the three desktop gates. Consider the separately labeled driver-fencing and unattended-expiry proposals before expanding autonomous workloads.

This candidate is deliberately submitted for the lead team to accept, amend, split, or discard. It has not been merged or deployed. Any rollout should use a pinned reviewed build and isolated canary state before production. The accompanying [technical design](HARNESS_CONTROL_DESIGN_2026-09-25.md) specifies the implemented contract, residual limits, and proposed next stages.
