# Harness control qualification evidence

These records were generated from local executions, not hand-edited test scores.

The same 26-case test source was compiled against baseline commit `43127402af2b82cd00561017570f350335593057` and the candidate. `baseline-source.json` and `validation-manifest.json` retain the shared test hash and implementation hashes. The baseline intentionally fails 22 cases; the candidate passes all 26. Twenty further consecutive repetitions passed.

`native-results.json` retains all 39 test outcomes, including three desktop failures. No tests were excluded from that run. Hostname, private directory paths and incidental fixture PIDs are not part of the public evidence.

Both SDK results exercised the real temporary frontend, mock provider, exact operator grant, isolated worker, pause/restart/resume and completed-effect receipt replay. They made no paid requests.

The local development build is not a production deployment or a substitute for final cross-platform/release qualification. See the [audit](../../HARNESS_CONTROL_AUDIT_2026-09-25.md) and [technical design](../../HARNESS_CONTROL_DESIGN_2026-09-25.md).
