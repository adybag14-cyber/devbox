# Bounded native text slicing

The C++23 `js_slice` helper preserves JavaScript UTF-16 index semantics for
execution-output excerpts, result budgets and job-log tails. Previously, even a
200-character prefix required converting the entire input into a temporary
UTF-16 string before converting the selected slice back to UTF-8.

The measured 0.9.33 implementation first checks only the requested prefix. If that prefix is
ASCII, its byte and UTF-16 indices are identical and the function copies the
requested slice directly. Bytes after the requested end cannot affect that
slice. Other input retains the existing conversion path. Empty or reversed
ranges return without conversion. No dependency or public tool is added.

Differential tests compare 10,854 ranges per run against the original full
UTF-16 conversion: ordinary text, embedded NULs, surrogate boundaries, malformed
and incomplete UTF-8, a long ASCII prefix followed by Unicode, deterministic
random bytes, oversized indices and reversed ranges. Baseline and candidate
core suites passed on Windows MSVC and Linux GCC.

## Measurements

The [complete data](benchmarks/text-slice-20261104.json) retains every trial,
build identity and executable hash. Initial measurements use A/B/B/A fresh
processes on both platforms, five trials per case per process. Each trial takes
128 slices from a 128 KiB input or 16 slices from a 4 MiB input, returning 200
UTF-16 units. Timed calls compare their bytes with the full-conversion reference.
Input construction, reference conversion, hashing and JSON output are excluded.

The initial Windows 128 KiB Unicode control showed a pronounced within-process
warm-up trend and a 93.9% median slowdown. A separate Windows A/B/B/A control uses
the same runtime implementations, a verified CPU-0 assignment, and at least
500 ms of per-case warm-up. It reduced that difference to 0.13% faster. This
controls the observed trend but does not establish which underlying scheduling,
frequency or allocator effect caused it. Original observations remain included.

The table uses that controlled Windows comparison and the original Linux
comparison. Comparisons are within each platform, not an operating-system ranking.

| Case | Calls | Windows baseline → candidate | Linux baseline → candidate |
| --- | ---: | ---: | ---: |
| 128 KiB ASCII, first 200 units | 128 | 19.489 → 0.01145 ms | 50.900 → 0.00704 ms |
| 128 KiB ASCII, final 200 units | 128 | 19.663 → 3.698 ms | 12.231 → 2.063 ms |
| 4 MiB ASCII, first 200 units | 16 | 123.344 → 0.00150 ms | 293.040 → 0.00075 ms |
| 4 MiB ASCII, final 200 units | 16 | 118.257 → 15.130 ms | 78.899 → 8.096 ms |
| 4 MiB with Unicode inside selected prefix | 16 | 118.031 → 121.433 ms | 79.460 → 78.265 ms |

For 16 ASCII slices of a 4 MiB input, requested allocation calls change from
608 to 16 on Windows and 384 to 16 on Linux. Requested bytes change from
413,680,896 to 3,328 and 469,769,504 to 3,216 respectively. These count cumulative
C++ allocation requests, not process RSS or an equivalent reduction in peak RAM.
All 480 timed trials across the original and controlled runs preserve reference
bytes and report zero retained allocation growth across the measured operation.

The Unicode-prefix fallback keeps its allocation behavior. Its controlled
Windows 4 MiB case is 2.9% slower; Linux is 1.5% faster. The change is justified
for bounded ASCII excerpts and tails, not as a universal Unicode speedup.
The workstation was shared, own CPU jobs were serialized for measurement, and
the pre-measurement CPU sample was 1%. Network, browser and model latency are
outside this benchmark. These measurements were collected without changing
either production service.

Initial sources: baseline `d1dc48809098272bbd3134b81535e5282e8ad3ea`, candidate
`92c3f203a07618a96d2441c7dc4b668afa190d20`. Controlled Windows sources: baseline
`aed44c843fefc10e034ecf04ce0d6f7019ade59d`, candidate
`2bf2c89be47b013cf61e7c112493ed7de2c62b2e`. The runtime source was verified
unchanged between the original and controlled versions on each side.
