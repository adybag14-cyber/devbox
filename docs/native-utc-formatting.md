# Native fixed-width UTC formatting

UTC timestamps previously passed through platform `time_t` conversion, calendar
functions and locale-sensitive streams. The new boundary fixtures fail against
that implementation on both Windows and Linux. The formatter also allocated ten
times per ordinary timestamp on the measured Windows build.

The C++23 implementation uses `sys_time<milliseconds>`, a floor to whole UTC days
and `year_month_day` conversion, then writes the fixed ASCII date/time digits.
Negative epochs therefore retain a nonnegative millisecond remainder within the
correct preceding day. It performs one allocation for the returned 24-byte string
and does not depend on `time_t` width or numeric locale. Years 0000–9999 match the
native parser's four-digit format; values outside that range now fail explicitly
with `UTC_TIMESTAMP_OUT_OF_RANGE` instead of emitting a malformed calendar string.

## Correctness

Windows MSVC and Linux GCC passed the UTC-format, core, jobs and state-store suites
at `f542818fc27436707c5467982e8a65e3ce45efda`. Coverage includes -1 millisecond,
negative whole seconds/days, years 0000/0001/9999, Gregorian century/leap boundaries,
10,000 deterministic round trips, integer-range rejection and a deliberately
grouped numeric locale. The core suite also checks the negative epoch and year2100,
so its existing cross-platform runtime lane covers the `time_t` boundary cases.

The retained baseline `ecca4de7e54059c85e52ab06ccde92a99021b52d` fails the new
Gregorian boundary fixture on both measured platforms. Baseline performance uses
only supported post-epoch inputs so correctness failures do not contaminate timing.

## Matched measurements

Each platform ran baseline/candidate/candidate/baseline in separate processes after
builds and tests. Every process ran five trials of 10,000 formats plus exact byte
comparisons for each of three date ranges. All normal-date output hashes matched.
For the range beginning on 1 November2026, median totals were:

| Platform | Before ms / 10,000 | After ms / 10,000 | Time reduction | Requested-byte reduction |
| --- | ---: | ---: | ---: | ---: |
| Windows | 12.488 | 0.414 | 96.68% | 88.89% |
| Linux | 2.855 | 0.240 | 91.59% | 95.40% |

[Raw trials, all three ranges, source identities and retained executable hashes](benchmarks/utc-format-20261101.json)
are preserved. Every timed trial ended with zero retained C++ allocation growth.
Input construction, expected-string generation and hashing are outside timing;
the per-result equality check is included. These are warm component measurements
on a shared workstation, not whole-agent or production latency measurements.
C++ allocation counters exclude direct C allocation and do not measure peak RAM.
