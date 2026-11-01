# Research evidence ownership experiment — held for timing diagnosis

This development branch avoids copying ledger sections discarded by summary
responses, borrows stored text during excerpting and moves completed response
arrays. Windows and Linux research, cache and offer tests pass. A private
100-source fixture exercises the real `research_evidence` API, including filesystem
reads and JSON parsing, for source pages, detail, re-excerpting, candidates and
exclusions. Every response digest matches the baseline.

The measured baseline is `02db7d54f39d16fdcea19a7f28bac01da22bf2bd`; the candidate
is `b83769774fc832887a6a975ad16ea25092ef39bd`. Each platform ran A/B/B/A, five
trials of twenty calls per mode per process. Requested C++ allocation bytes fell
roughly 9–21%, but elapsed results were mixed. The first comparison included a
10.54% Linux detail slowdown and a 7.03% Linux source-page slowdown. A follow-up
pinned Windows logical processor 16 and Linux guest vCPU 0 before child startup.
Windows results improved, but the Linux follow-up showed regressions of 9.70–29.42%.
Guest affinity does not pin a WSL vCPU to a physical Windows processor.

Both [initial trials](benchmarks/research-evidence-20261101.json) and
[affinity-control trials](benchmarks/research-evidence-affinity-20261101.json)
are retained with source identities and executable hashes. These results establish
lower requested C++ allocation volume for this fixture, not a general timing
improvement. They do not measure peak RAM, network or model time. No trials have
been discarded. This branch is held for further diagnosis and is not a selected
release candidate or a production deployment.
