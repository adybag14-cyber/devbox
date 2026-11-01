# Reusing document detection work

Web extraction checked the same immutable response for challenge markers both
before parsing and at the end. The first positive check already returned, so
the second raw-body check could not add evidence. It repeatedly lowercased up
to 128 KiB and compiled the same title pattern again. Charset detection also
recompiled fixed patterns for each document.

The C++23 extractor retains the initial raw-response check and the later checks
of the newly extracted title and text. Fixed RE2 patterns are now shared as
const objects with thread-safe static initialization. Meta-only charset
detection borrows the same bounded input prefix instead of copying it. Marker
patterns, prefix limits, case conversion and classification rules are unchanged.

The existing Windows and Linux encoding, metadata, research and offer suites
cover blocked/challenged responses, HTTP 202 search challenges, meta-only legacy
charsets and extracted evidence. The same twelve-case document benchmark used
for encoding compares saved baseline and candidate binaries in A/B/B/A order,
after builds finish. Every output digest must match. Measurements include
complete extraction and exclude network, browser, model time and peak live RAM.

## Compiler-layout investigation

The initial candidate reduced allocations but made several large Linux cases
slower. Opposite-order BAAB runs and CPU-0-pinned ABBA runs reproduced that
regression. Disassembly showed identical scalar UTF-8 validator instructions at
different addresses, with the ASCII loop crossing a 64-byte instruction boundary
in the slower binary. A GNU x86 function-alignment hint moved that loop back
within the boundary and removed the large ASCII regression. The hint adds no
instruction-set requirement and leaves other compilers/architectures unchanged.
Validation logic remains identical.

The [initial measurements](benchmarks/web-scan-initial-20261031.json) and
[all nine diagnostic runs](benchmarks/web-scan-diagnostics-20261031.json) are
retained. This is evidence for the measured compiler and host, not a claim that
cache alignment always produces a particular speedup.

## Final matched results

Baseline `248d26cb7c8909756c646940f8d5d14e9fa5f2fc` was compared with revised
candidate `20d526c80423b2ecabeeaefa11b70cbb9b606a1e` in fresh A/B/B/A runs on each
platform. All output digests matched. Windows and Linux both passed the encoding,
metadata, research and offer tests for this candidate.

| Fixture | Platform | Baseline ms / 10 | Candidate ms / 10 | Time reduction | Requested-byte reduction |
| --- | --- | ---: | ---: | ---: | ---: |
| 128 KiB HTML | Windows | 5.10 | 3.30 | 35.4% | 47.7% |
| 128 KiB HTML | Linux | 3.62 | 2.35 | 35.0% | 48.0% |
| 4 MiB ASCII | Windows | 35.90 | 34.48 | 3.9% | 46.4% |
| 4 MiB ASCII | Linux | 27.22 | 25.41 | 6.7% | 41.7% |
| 4 MiB HTML | Windows | 68.42 | 57.64 | 15.7% | 47.7% |
| 4 MiB HTML | Linux | 46.05 | 48.02 | **-4.3%** | 48.0% |

The remaining Linux HTML slowdown is retained explicitly. The large legacy
control was effectively flat on Linux (-0.03%). The benefit is lower allocation
traffic and improved common small-page extraction, with the displayed large-page
tradeoff; this is not a universal latency win. C++ requested bytes include freed
temporaries and exclude direct C allocations.

[All final trials](benchmarks/web-scan-20261031.json) include every size and
encoding case, source identity and retained baseline hash.
