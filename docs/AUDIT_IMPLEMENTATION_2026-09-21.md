# Backend audit implementation: 21 September 2026

Authority: the user supplied `Devbox-Hermes-Audit-Report-and-Plan-2026-09-21.zip` (SHA-256 `cc8b9e04911a0d07c1492f3d548cfbb294e300058ebb5c2ac8fbc5c065c735d2`) and explicitly requested **all backend tickets**, including H12–H14. The full technical report and all 18 backlog entries were read before implementation. Hermes approval/run-monitor UI integration is excluded. ChatGPT's website and Devbox MCP remain the existing client interface.

The audit's verifier reproduced the input ZIP's 382 verified member hashes, 778 tool records, 15,292 HTTP records and six admission timeout messages. Performance estimates in the report are hypotheses, not achieved improvements.

## Implementation sequence and acceptance map

| Ticket | Backend work | Required evidence |
|---|---|---|
| H01 | Aligned wall/steady handling timestamps, connection/receive boundaries, microsecond phases, build identity and outcome taxonomy | Deterministic 10-second idle reuse; clock-adjustment classification; correct waits/application exits |
| H02 | Canonical schema/alias/permission/platform registry and generated counts/docs | Existing 50 names preserved; legacy wire parity; no stale 47 count |
| H03 | Isolated paired benchmark harness with pinned build, workload/cache/host provenance and uncertainty | At least 1,000 short-operation samples per class across repeated runs; no production load |
| H04 | Explicit trusted-operator and restricted worker profiles; principal/run/operation grants, target identity, expiry/revocation and egress | Adversarial path/reparse/argument/substitution tests; denied operations have no effects |
| H05 | Payload-free telemetry, controlled child environments, scoped secret references and durable security audit | Canary secrets absent from unauthorized channels; bounded spool and explicit failure policy |
| H06 | Response/SSE deadlines, aggregate byte budgets, bounded worker/process shutdown and resource limits | Slow-reader/fanout/stuck-worker tests; foreground containment and durable-runner survival |
| H07 | Exact-artifact qualification/promotion, signed provenance, resolved dependency notices/SBOM and security lanes | Matching tested/packaged/deployed digests; fail-closed verification; qualified toolchain cache keys |
| H08 | Transactional indexed state, one writer, fencing, migration/export and durable operation identity | 10k/100k bounded pages/admission; crash-boundary recovery and downgrade protection |
| H09 | Completion-driven deadline waits and authenticated scheduler notifications/resource vectors | Fairness/PID-reuse/lease tests; matched handoff and fast-path measurements |
| H10 | Resumable content-addressed uploads and one final CAS publish; bounded directory traversal | Replay/crash tests; linear byte work; declared traversal memory bound |
| H11 | Profile-led allocator/observer changes and optional representative PGO experiments | Keep only demonstrated paired improvements; retain truthful metric precision |
| H12 | Direct provider/local inference adapters with typed capabilities, streaming, cancellation and budgets | Recorded/mock provider traces, malformed fragments, cancellation/rate limits and secret isolation |
| H13 | Durable backend run/approval/controller state, outbox and uncertain-effect reconciliation | Restart at each transition; no duplicate admitted effects; explicit reconciliation and terminal cancellation |
| H14 | Harness task-quality and adversarial evaluation | Per-task outcomes/confidence, prompt-injection/isolation corpus and budget/loop circuit breakers |
| H15 | Negotiated MCP Tasks plus artifact/evidence resources and workflow instructions | Modern/legacy negotiation, reconnect/cancel/input flows; **no Hermes UI** |
| H16 | Research provider boundary, HTTP-date Retry-After, evidence/egress/coverage quality | Freshness and failed-source behaviour; existing address/TLS/redirect/byte protections |
| H17 | Structured WSL distribution/path execution and accurate platform support states | Independent platform qualification; Windows input protections; no untested mobile/desktop parity claims |
| H18 | Read-only/disposable canaries, state fencing, admission drain, exact-artifact promotion and rollback | No lost acknowledged admission or duplicate effects; qualified C++ rollback and full release gates |

## Guardrails

- Work in an isolated checkout; do not edit source while a build is running. Use frozen source and clean builds for final qualification.
- Preserve the live trusted-operator workflow. New autonomous profiles, providers and external connections must use explicit configuration/grants; models cannot enlarge their own authority or budgets.
- Keep the current native networking, cancellation, durable receipts, process ownership, desktop observation and compatibility protections.
- Use a dependency-directed modular C++23 core and selected isolated workers. Do not build an inference engine or add a web UI.
- Do not claim unsupported OS isolation, unmeasured speedups, universal search coverage or unperformed physical-device certification.
- Qualify changes locally and in hosted gates, then promote the exact tested artifact through the existing ownership-aware deployment workflow. Preserve Guardian, the tunnel, credentials, unrelated jobs and recovery data.

Progress and acceptance evidence are tracked in `audit-backlog-progress.json`. A ticket is complete only when its implementation and applicable acceptance evidence are present; a declared unsupported capability is not a claim that a backend was implemented.

## Filesystem worker lifecycle checkpoint

Host file reads, writes, CAS checks, bounded listings, file inspection and wait observations execute in an owned C++23 worker. Requests and responses have independent byte caps; the parent collects JSON in a bounded byte buffer. Cancellation and deadline escalation terminate only that child. Windows workers have a 256 MiB Job Object limit and a one-process limit (two for the explicitly configured PowerShell inspection child). This isolates lifecycle failures while retaining existing trusted-operator file authority. A missing acknowledgement never proves a write did not occur.

Windows passed five focused native tests including real frontend shutdown during a deliberately stalled file worker; Linux passed its filesystem/engine tests. Both passed the 52-tool SDK fixture including exact bytes, durable retry, cancellation and 32 concurrent waits. A deadline-edge regression discovered by the engine test was fixed: an expired file wait returns a timeout observation, with unknown existence when no observation completed. See `docs/audit-evidence/H06-filesystem-local.json`. Artifact assembly and legacy metadata maintenance remain under H06/H08 integration; production is unchanged.

## Indexed maintenance checkpoint

SQLite mode no longer builds a filesystem-wide job index or copies/sorts all quota entries. Maintenance visits at most 64 indexed jobs per call and persists per-job disk samples, aggregate charges and its continuation cursor in one transaction. Subsequent calls resume the cursor after frontend restart. The monitor continues incomplete cycles promptly, then returns to its idle cadence. Accounting is explicitly sampled/partial; it is not an OS disk quota. Reclamation uses eligible terminal jobs in page order. Interrupted or explicitly unverified outcomes are retained.

Job artifact inspection, compaction and reclamation run in the cancellable filesystem worker. A job directory admits its normal files plus the known research ledger/document subtree, capped at 1,024 entries in total; unexpected nested/alias content is retained and reported. Pruning marks metadata as lacking artifacts and never removes the admitted operation receipt. Windows and Linux passed maintenance, indexed-job and legacy-job tests, and their SQLite SDK profiles. The 140-job maintenance fixture verifies bounded pages, resumed cursors, aggregate replay integrity, quota convergence, retained receipts, unexpected-content refusal and cancellation. See `docs/audit-evidence/H08-maintenance-local.json`. State export/recovery and resource leases remain outstanding.

The shutdown follow-up removes the remaining blocking POSIX `waitpid` from foreground cleanup. A fixed-capacity reaper slot is reserved before launch; unobserved exits are collected asynchronously without further PID signals. Shutdown of that reaper is bounded even if a kernel-stalled child has not exited. Windows no longer adds an undeclared three seconds after its grace period. A timed-out/cancelled job whose child exit was not observed is recorded as interrupted with unverified workload termination. Foreground/process/filesystem/job checks passed on Windows and Linux, including durable runner behavior and the 4,096-slot reaper bound. Kernel-level stuck-I/O injection remains outside this fixture evidence.

## ChatGPT website baseline and research regressions

At the user's direction, live evaluation uses the signed-in ChatGPT website (6 Pro) and Devbox Hermes plugin. The current production baseline passed serving-identity discovery and an independently checked synthetic file edit, including integer arithmetic, Unicode preservation and rejection of an injected source instruction. Research returned two complete live retailer-reported offers but missed the 50-source target: the agent deduplicated 23 consulted documents after follow-ups, with a native 120-second time-budget stop. A single baseline is not evidence of a general success rate or speed improvement.

Contract 8 still has 52 tools. The new C++23 regression fixes add exact Amazon ASIN source identity, a bounded matched-buybox adapter, explicit upstream-cache-age reporting, colour/URL consistency warnings and optional `product_targets`. Every included model/capacity phrase must occur in the same title or individual offer name; explicit exclusions reject sibling models. `require_offer` demands an extracted offer name instead of a title-only match. Per-target counts describe matched source documents, not independently verified prices. Old extraction caches are invalidated.

Windows and Linux passed offer, research and engine tests plus the SQLite SDK profile. All four frozen schema profiles passed. Replaying three private cached baseline documents recovered two distinct ASIN offers with their recorded amount, condition and stock. Those are frozen regression results; they are not newer live checks. Private conversation URLs and full retailer snapshots stay outside the public repository. Candidate browser replay remains required after safe promotion. See `docs/audit-evidence/H16-browser-regressions-local.json`.

## Scheduler notification and cleanup recovery checkpoint

Scheduler waits now subscribe to same-process changes and OS notifications from a private `.notifications` directory. Windows uses ReadDirectoryChangesW, Linux uses inotify, and macOS uses kqueue. Wake-ups are hints; admission still rechecks fairness, capacity and stored owner identities. A 500 ms reconciliation deadline covers missed notifications. The private child directory preserves compatibility with existing execution roots. Windows closes each short-lived signal writer to ensure timely LAST_WRITE delivery.

Two sequential 1,000-observation qualification runs measured release-to-cross-process-grant p95 of **2.790 ms on Windows** and **0.446 ms on Linux/WSL** (p99 3.274 ms and 0.586 ms). Both meet the 20 ms qualification target on this development host. This is not a full benchmark or release-performance certification; resource vectors and indexed leases remain open. See `docs/audit-evidence/H09-notifications-local.json`.

Artifact pruning now records a durable intent before deleting files. An actual worker-exit fixture interrupted deletion after status.json was removed; the next reconciliation completed cleanup while retaining operation identity. Known research subdirectories are counted and reclaimed, unexpected subtrees remain protected, live producers are retained, and pruned/pending log requests fail explicitly. Windows and Linux native checks and SQLite SDK profiles passed.

Hosted qualification of commit `7416819fe454a9783b463fbf51d8e1a8fe9def29` completed successfully in run `35778153484`: all 18 native/platform/package/certification jobs passed, including Windows managed startup, Guardian and rollback checks. Later commits require their own hosted qualification. Production remains unchanged.

Shared memory/GPU/disk reservation dimensions are now implemented and tested on Windows and Linux. The backend run/Tasks SDK fixture passed on both with an actual isolated effect and frontend restart. Reservation scope and disabled dimensions are explicit; indexed lease integration and broader performance comparisons remain open.

Hosted run `35782649031` at `751ec9e` found a provider-pacing timing failure on Linux GCC and a legacy fallback queue-timeout classification failure on macOS x86_64; its distro failures followed the missing Linux package. Discovery now serializes each provider request and spaces subsequent requests from an observed completion boundary, retaining the strict timing assertion. The fallback maps queue-head deadline expiry to its established typed queue timeout with phase details. Native pacing and the 15-test fallback scheduler suite passed locally (Windows 15 passes; Linux 11 passes and four Windows-only skips). Amazon buybox extraction also rejects primary/companion currency disagreement. Later hosted qualification is still required.

## Export and fenced recovery checkpoint

The native operator CLI now provides payload-free state diagnostics, explicitly opted-in private SQLite snapshots, and digest-verified restoration into a new, write-fenced directory. The online backup includes ordered events and operation identities but leaves external artifacts and broker credential/provider configuration files separate. The default report uses a fixed field/kind allowlist and excludes payloads and record identifiers. A restored snapshot never becomes a runnable effect source automatically and cannot overwrite live state. Windows/Linux snapshot and coordinator tests passed, plus a Windows CLI smoke checking the private-data flag. See `docs/STATE_EXPORT_AND_RECOVERY.md` and `docs/audit-evidence/H08-snapshot-local.json`. Full recovery/reactivation and release rollback drills remain under H18.

Hosted run `35788370726` at `f4ee532` passed Linux, Android/Termux and the prior timing regressions, but found a 261-character Windows scheduler temporary path and macOS concurrent notification-file rejection. Atomic JSON temporary names now have a fixed short basename in the destination directory; a Windows 230-character final-path regression passed. macOS permission/type/link checks remain strict and now report bounded errno/mode/owner/link diagnostics to identify the platform failure. Hosted confirmation is pending.
