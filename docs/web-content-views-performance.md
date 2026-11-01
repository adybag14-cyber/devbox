# Borrow DOM attributes during content selection

The article-selection walk copied every DOM attribute into a temporary string,
concatenated class and id values for token tests, and parsed the same source URL
again for each candidate permalink. Pages with many attributed elements and
links repeated these allocations throughout selection and exclusion.

The C++23 walk now borrows attribute bytes from the live DOM. These views remain
inside the synchronous walk; none escapes in the returned selection. Whole-token
class/id expressions run separately on each attribute, preserving the space
boundary previously introduced by concatenation. The source URL is parsed once
per selection and reused for permalink resolution and index-path detection.

Windows MSVC and Linux GCC passed the research and offer tests. Added cases
verify declared article bodies in either class or id, empty class attributes,
noise-token boundaries, and relative, fragment and absolute permalinks. Existing
hidden-content, ambiguous-article and independently scoped offer checks remain.

## Matched full-extraction measurements

Baseline `afae4346f0536af3b1e9ed186bce4ee83c462fed` and candidate
`a6f72ff36764a6c1fceb2f4b89288a28c3653664` ran in A/B/B/A order on each platform,
after builds completed. Each process ran five trials of twenty full article
extractions for six cases: 16, 128 or 512 paragraphs, with sparse or long
attributes and relative permalinks. Every complete output digest matched.

| 512-paragraph fixture | Platform | Before ms / 20 | After ms / 20 | Time reduction | Requested-byte reduction |
| --- | --- | ---: | ---: | ---: | ---: |
| Sparse attributes | Windows | 43.04 | 41.70 | 3.12% | 2.36% |
| Long attributes | Windows | 60.22 | 52.56 | 12.71% | 21.98% |
| Sparse attributes | Linux | 31.58 | 29.71 | 5.93% | 1.72% |
| Long attributes | Linux | 58.71 | 49.73 | 15.29% | 20.08% |

[Raw trials, source identities and retained executable hashes](benchmarks/web-content-views-20261101.json)
include every case. These are warm synthetic component measurements on a shared
workstation. Fixture creation and output serialization/equality checks are not
timed; parsing, content selection and extraction are timed. Requested C++ bytes
include freed buffers and exclude direct C allocations, so they are not peak RAM
or the complete DOM allocator cost. Network, browser and model latency are not
measured, and the percentages are not additive with other optimizations.
