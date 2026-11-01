# Incremental JSON-LD metadata budgeting

For every accepted structured record, document extraction serialized the entire
previous metadata array again just to learn its byte length. A catalogue with
many records therefore repeated the same work and allocations for each prefix.
The check also omitted the next comma, allowing a one-byte budget overrun.

The C++23 extractor now tracks the serialized size of the metadata array as
records are appended. It starts with the two array delimiters, serializes each
candidate item once, and includes the separator when the array is nonempty.
The counter spans all JSON-LD scripts in the same document. Rejected items do
not consume the remaining budget, so later smaller records may still fit.
The 64-record cap, traversal bounds, source paths, per-field clipping and offer
extraction are retained. Article/WebPage/BlogPosting attribution is computed
before accepting the item, so those final fields are included in the byte budget.

## Validation

The baseline failed the new exact-boundary regression by accepting an array
whose encoding was 32,769 bytes. The candidate accepts an exactly fitting
32,768-byte product-metadata array and rejects the next byte, while retaining
an explicit truncation signal. Both one JSON document and multiple HTML script
elements are covered, including escaped quotes, UTF-8 text and a later smaller
record. Windows MSVC and Linux GCC passed the metadata, encoding, web-research
and offer tests.

## Matched measurements

Baseline `727247f4af598efa952d902892995adde2f0c84e` and candidate
`1cd43466f4f32ba374e9ae6f386965c0229a06d8` were built cleanly and retained.
Each platform ran the binaries in A/B/B/A order, after builds completed.
Each process tested six combinations: 8, 32 or 64 product records across one or
eight JSON-LD scripts. Every case had five trials of twenty complete document
extractions. All in-budget output digests matched.

| 64-record fixture | Platform | Baseline ms / 20 | Candidate ms / 20 | Time reduction | Requested-byte reduction |
| --- | --- | ---: | ---: | ---: | ---: |
| One script | Windows | 44.93 | 12.44 | 72.3% | 74.8% |
| One script | Linux | 40.22 | 10.17 | 74.7% | 70.6% |
| Eight scripts | Windows | 45.23 | 12.64 | 72.1% | 74.7% |
| Eight scripts | Linux | 39.74 | 10.54 | 73.5% | 70.5% |

[Raw trials, build identities and binary hashes](benchmarks/web-metadata-20261031.json)
include the smaller cases as well. The benchmark uses synthetic product
documents on a shared host. C++ allocation counters include freed temporaries
and exclude direct C allocations. Network transfer, browser rendering, model
time and production peak RAM are not measured.
