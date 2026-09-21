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
