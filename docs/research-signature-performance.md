# Streaming research similarity signatures

Near-duplicate detection compiled the same Unicode tokenizer for every source,
retained every normalized word, then revisited five consecutive words for each
hash. The C++23 implementation now shares the immutable tokenizer and keeps a
five-word rolling buffer. The order of bytes and separators in every hash is
unchanged. Significant numeric/negation tokens, word counts, sorting, duplicate
hash removal and the 2,048-smallest-hash sampling policy are retained.

The implementation is isolated in a private source header/module so its results
can be compared directly. This adds no MCP tool or runtime dependency. The old
full-word algorithm is retained only as a test oracle. Differential tests cover
empty input, short windows, punctuation, multilingual text, long words, numeric
and negation tokens, and sampling beyond 2,048 unique hashes. The research suite
also verifies that syndicated copies do not inflate source counts while changed
numeric facts remain separate reports. Windows MSVC and Linux GCC passed both
the signature and complete research suites.

## Matched measurements

Baseline `eba8af3f2cbb2ea3999f7d8e4b0e931ec8589546` and candidate
`d161703b5296f377775f3d4e428a8b8d06d82ea8` ran in A/B/B/A order on each platform.
Each process ran five trials of 100 signatures for four cases: approximately
4 KiB or 64 KiB with short or long words, mixed Unicode, numbers and negations.
Every signature digest matched across versions, including all retained hashes,
significant tokens, counts and sampling flags.

| 64 KiB input | Platform | Before ms / 100 | After ms / 100 | Time reduction | Requested-byte reduction |
| --- | --- | ---: | ---: | ---: | ---: |
| Short words | Windows | 423.99 | 161.60 | 61.89% | 88.52% |
| Long words | Windows | 493.99 | 110.29 | 77.67% | 92.48% |
| Short words | Linux | 414.66 | 157.38 | 62.05% | 90.58% |
| Long words | Linux | 333.85 | 108.31 | 67.56% | 94.02% |

[Raw trials, executable hashes and source identities](benchmarks/research-signatures-20261101.json)
include every smaller-input case. These are warm component measurements on a
shared workstation, after builds/tests finished. Input construction and output
serialization/equality are outside the timed interval. First initialization of
the constant tokenizer is outside the warm trials. Requested C++ allocation
bytes include freed buffers and exclude direct C allocations; they are not peak
RAM. The shingle and significant-token results still scale with input, so the
whole algorithm is not claimed to use constant memory. Network, model and
end-to-end research latency are not measured.
