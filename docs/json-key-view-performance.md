# Bounded JSON key lookups

The native `json_string`, `json_bool`, `json_uint` and `json_number` helpers now
pass their existing `std::string_view` key directly to the ordered JSON parser's
heterogeneous lookup. They no longer allocate a temporary `std::string` for each
long field name. Defaults, null handling, numeric validation and result ownership
are unchanged. Keys remain length-bounded; they need not be null-terminated.

The `json-key-views` regression covers empty, UTF-8 and embedded-NUL keys, views
inside larger strings, missing fields, non-object inputs, explicit nulls and
wrong types. Its allocation fixture performs 8,000 successful long-key lookups:
the previous implementation reports 8,000 temporary allocations, and the updated
implementation reports zero. This does not require string result values of
arbitrary length to be allocation-free; returned strings still own their contents.

The optional `devbox-json-lookup-bench` target reports time, allocation calls,
requested bytes and retained allocation change. On 28 October 2026, a controlled
MSVC Release comparison used the implementation at
`50333d43bab83bc55cade6979525620d320346f8` as baseline, three processes per variant
in alternating order, and five trials per phase in each process. Each trial made
200,000 lookups. Median nanoseconds per lookup across the 15 trials were:

| Lookup | Before, ns | After, ns | Allocations before | Allocations after |
| --- | ---: | ---: | ---: | ---: |
| Long string key | 43.51 | 20.56 | 1 | 0 |
| Long unsigned-integer key | 43.19 | 19.85 | 1 | 0 |
| Long boolean key | 38.80 | 13.10 | 1 | 0 |
| Long numeric key | 39.08 | 14.57 | 1 | 0 |
| Long key with null value | 37.36 | 10.79 | 1 | 0 |
| Missing long key | 33.77 | 7.98 | 1 | 0 |
| Short unsigned-integer key | 21.14 | 19.50 | 0 | 0 |

The long key contains 96 bytes, and the returned string in this fixture is short.
Each old long-key lookup requested 112 temporary bytes on this standard library.
Every phase ended with zero retained allocation growth in both versions. These
are component measurements on one machine, not a whole-server speedup or an
all-path memory-leak result. Core, telemetry, state-store, indexed-maintenance and
scheduler tests also passed on Windows after the change.
