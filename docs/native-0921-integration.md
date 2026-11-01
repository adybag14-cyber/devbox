# 0.9.21 integration candidate

This candidate combines the prepared changes from PR149–PR154 against merged
0.9.15 main, including the Windows native directory-publication fix identified
by PR151's CI. The published 0.9.14 artifacts and selected 0.9.15 release source
remain immutable. The earlier feature PRs are superseded only when this combined
candidate has passed its own qualification and merged.

Included changes and retained evidence:

- [Chrome response ownership](chrome-response-ownership.md): move owned response
  buffers and image data instead of retaining an extra image-sized copy.
- [Web encoding](web-encoding-performance.md): strict allocation-free UTF-8
  validation and borrowed original response bodies.
- [Metadata budgeting](web-metadata-performance.md): linear byte accounting and
  the one-byte separator-boundary correction.
- [Document scans](web-document-scan-performance.md): shared detection patterns,
  removal of a duplicate scan, and the measured x86 alignment correction. The
  remaining Linux large-HTML timing tradeoff is explicitly retained.
- [Android artifact binding](android-artifact-binding.md): staged package bytes
  must match the completed cross-compilation's header and binary binding.
- [Readiness recovery](readiness-recovery-evidence.md): capture fast readiness
  failures/recovery and count confirmed health replacements without weakening
  identity checks or durable-work drain refusal.
- [Native publication](native-directory-publication.md): bounded Windows
  directory-rename retries while preserving atomic publication and existing
  destinations.

Per-feature benchmarks are scoped to their recorded source/build identities and
workloads; their percentages must not be added together or presented as measured
production speedups. Full combined-source CI and release acceptance are separate
requirements. No interpreter, container runtime or paid service is added to the
C++23 production runtime.
