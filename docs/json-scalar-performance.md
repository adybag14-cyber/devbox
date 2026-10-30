# Native JSON scalar formatting

The native compact encoder previously constructed the general JSON serializer
for every integer, boolean and null. Signed/unsigned decimal integers now use
`std::to_chars` and a 32-byte stack buffer; booleans and null append their fixed
JSON literals directly. Floating-point, binary and Unicode/escaping behavior
continues through the existing reference paths. No dependency was added.

The byte-parity tests include INT64_MIN, INT64_MAX, UINT64_MAX, seeded full-width
integers, nested values, special floats, binary values and malformed UTF-8 under
strict/replacement/ignore policies. Windows MSVC and Linux GCC core/engine tests
passed. Every benchmark fixture also matched the independent reference output
and retained the same output hash before and after.

Measured code: baseline `40780972540c898fbdfd2bada773395d5d6fcfff`, candidate
`2f86268b829af09cf8891e9ea42d0f4f178b799d`. The later documentation commit does
not replace these artifact identities. Full source, compiler, binary hashes,
individual trials and cleanup receipts are in
[the evidence record](benchmarks/json-scalars-20261030.json).

## Isolated serializer results

Medians of five trials, each serializing 200 responses:

| Platform and fixture | Before | After | Allocation calls, before → after | Requested bytes, before → after |
| --- | ---: | ---: | ---: | ---: |
| Windows numeric status | 12.8893 ms | 3.4182 ms | 182,800 → 1,200 | 55,081,800 → 4,333,000 |
| Linux numeric status | 8.2302 ms | 1.7910 ms | 182,400 → 800 | 52,462,200 → 3,072,800 |
| Windows browser controls | 10.6348 ms | 5.3626 ms | 155,800 → 46,200 | 40,925,000 → 10,237,000 |
| Linux browser controls | 7.2265 ms | 2.9435 ms | 155,400 → 45,800 | 38,635,800 → 8,769,800 |
| Windows large ASCII output | 7.2205 ms | 5.4817 ms | 1,200 → 400 | 13,755,000 → 13,531,000 |
| Linux large ASCII output | 0.8186 ms | 0.8614 ms | 1,200 → 400 | 13,737,200 → 13,519,200 |

Numeric-status serialization used 99.3–99.6% fewer C++ allocations and took
73.5–78.2% less elapsed time in these fixtures. Browser-control serialization
also improved. The large ASCII case had a small Linux timing increase despite
fewer allocations. All microbenchmarks had zero retained C++ allocation growth
across their measured loops; that is not a whole-server leak-freedom result.

## End-to-end limits

Four fresh Windows loopback MCP processes per source exercised 100 detailed
status and 100 compact status calls, alternating action order. The first series
did **not** establish an HTTP speedup: median detailed status was 885.0 → 911.4 ms
and compact status was 521.1 → 619.5 ms per 100 requests. Allocation reductions
were much smaller (0.32% and 1.45% respectively).

A follow-up reused the exact retained executables in A/B/B/A order, with balanced
action ordering. Detailed status was 886.7 → 898.8 ms and compact status was
472.0 → 497.9 ms per 100 calls. Individual compact-status samples varied from
390.1 to 559.3 ms. These shared-workstation timings remain too variable to claim
an end-to-end latency improvement, and the observed increases are retained in
the record rather than discarded.

The source explains the limited coverage: responses with conservative memory
charges above 64 KiB use `bounded_json_dump` on the codec pool. Both status
responses take that separate, unchanged bounded-stream path. This change
improves the compact encoder used by smaller responses and other direct callers;
it does not optimize the bounded serializer or remove status collection costs.

Each HTTP trial finished ready with zero live connections. The exact recorded
coordinator process instance was verified exited. Allocation counters include
the benchmark client, server and background C++ work, and exclude direct C
malloc. Timings are not public-network or model latency. No production binary
was changed during these experiments; full CI remains a separate gate.
