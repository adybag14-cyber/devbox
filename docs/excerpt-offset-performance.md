# Bounded UTF-8 excerpt offsets

Every research excerpt previously allocated a `size_t` offset for every code
point in its source text. A short excerpt from a 64 KiB document therefore
created a much larger temporary mapping than the returned evidence. A missing
whole-query match also compiled the same Unicode word-tokenization expression
again on every call and retried an identical single-word query.

The C++23 implementation counts code points without an offset array, maps ASCII
positions directly, and walks only the selected UTF-8 window from its matched
position. It counts the nearer prefix or suffix to locate a non-ASCII match.
The constant Unicode tokenizer is shared across calls, and an identical failed
whole-query lookup is not repeated. Query priority, clipping, original byte
offsets, Unicode code-point offsets and output fields remain unchanged.

## Validation and measurements

Windows MSVC and Linux GCC passed the excerpt, research and offer tests. Explicit
expected-value cases cover bounded string views, Greek text, emoji, Kelvin-sign
case folding, multi-word fallback, no match, empty input and zero/full limits.

The retained baseline is `dc42f42700e499ee39ebcc6815e731e8b72b20e5`; the measured
candidate is `88c29a0482de356c63941ec0e35e66b6c1cbf946`. Each platform ran baseline,
candidate, candidate, baseline after its builds finished. Each process ran five
trials of 100 excerpts for each of twelve cases: ASCII/multilingual, about
4 KiB/64 KiB, and lead/found/missing queries. Every output digest matched across
both versions. The table gives medians per 100 excerpts for the 64 KiB ASCII
cases; raw evidence includes all trials and all smaller/multilingual cases.

| Selection | Platform | Before ms / 100 | After ms / 100 | Requested-byte reduction |
| --- | --- | ---: | ---: | ---: |
| Lead | Windows | 9.51 | 2.93 | 98.70% |
| Found query | Windows | 14.68 | 7.85 | 93.01% |
| Missing query | Windows | 271.21 | 6.44 | 99.46% |
| Lead | Linux | 2.92 | 0.97 | 99.00% |
| Found query | Linux | 5.90 | 3.95 | 92.95% |
| Missing query | Linux | 238.52 | 2.86 | 99.44% |

[Final raw trials, source identities and executable hashes](benchmarks/excerpt-offsets-followup-20261101.json)
are retained alongside the [first-stage measurements](benchmarks/excerpt-offsets-20261101.json).
The first stage removed the mapping allocation but exposed the repeated-tokenizer
cost and retained some slower missing-query trials. Those observations prompted
the tokenizer follow-up; they have not been discarded.

These are warm component measurements on a shared workstation. Fixture creation,
output hashing and serialization for equality checks are outside the timed
interval. First-use initialization of the shared tokenizer is outside the warm
trials. Allocation counters measure requested C++ bytes, including freed
temporaries, and exclude direct C allocation; they are not peak RAM. No network,
browser, model latency or production speedup is claimed.
