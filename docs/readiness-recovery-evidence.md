# Readiness failure and recovery evidence

The production Windows frontend returned several fast HTTP 503 readiness
responses around 20:22 UTC on 31 October 2026, then appeared under a new process
generation at 20:23 with the same verified 0.9.14 binary. The native status still
reported zero restarts. Existing incidents only recorded event-loop delays, so
they did not preserve the underlying readiness failure. The exact original
store/scheduler failure cannot be reconstructed from those retained records.

This 0.9.21 candidate corrects those reporting gaps in the C++23 runtime:

- The existing incident worker records readiness degradation and recovery even
  when the event loop is fast. Transitions bypass the previous incident's
  cooldown; repeated unchanged failures retain the existing rate limit. The
  existing bounded log carries scheduler/store snapshots and failing-worker
  details alongside process generation and active-work evidence.
- The native supervisor retains the failed probe phase, outcome category, HTTP
  status, duration and exact child identity. Raw response bodies and credentials
  are not copied into this diagnostic.
- A frontend stopped after a confirmed health drain increments restart
  accounting and records the previous child and triggering probe. A refused or
  unconfirmed drain does not count as a replacement. The existing process
  identity and durable-work checks still govern every stop.
- A locked health diagnostic log is reported in native state instead of
  interrupting supervision. Recovery retains the earlier failed-probe evidence.

These changes expose the cause and history of recovery; they do not claim to
have identified the missing production failure or to eliminate every possible
readiness outage. Production artifacts are unchanged by source development.

## Validation

Windows MSVC and Linux GCC builds at
`5a98d50113f3a65be7192f13aba3692e3b56f168` passed the engine and native-management
tests. A real background readiness failure produces an incident despite low
event-loop timings, and recovery produces another inside the 30-second cooldown.

Both complete native lifecycle smoke suites passed in private fixture roots.
The Linux suite additionally suspends its identity-checked frontend and proves
that unconfirmed work is preserved. It then makes only the idle fixture's
scheduler path unavailable, verifies an acknowledged health replacement, checks
the retained 503/cause/child identity and restart count, restores the path, and
requires recovery with the existing companion preserved. Ordinary startup,
active-work refusal, crash recovery, rollback, explicit stop and durable effects
remain covered. No production process was stopped by these tests.
