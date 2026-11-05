# Native browser opacity ancestry

The C++23 browser's synchronous DOM observation used to walk and read computed
opacity for every text node and readable control separately. A page with many
text leaves under the same deep wrapper hierarchy repeated the same ancestor
work thousands of times.

The ancestry change developed in 0.9.37 and included in the 0.9.40 integration
memoizes opacity ancestry during one observation. A positive cache
entry records the number of elements to the composed root; a negative entry
records a proved zero-opacity or over-depth path. Keeping the depth preserves
the existing 256-element bound. An incomplete walk that hits the bound does not
cache an unproved result for its shorter ancestor suffixes. The cache holds at
most 8,192 weak keys and is discarded at the end of the synchronous call.
No computed styles, nodes or decisions are reused across separate observations.
No permission, action, schema or runtime dependency is added.

## Local comparison

[Raw measurements](benchmarks/observation-ancestry-20261104.json) retain all 320
timing samples, script hashes, reference output hashes and browser identities.
The benchmark extracts the actual embedded observation from the baseline and
candidate C++ sources and runs both on the same page. Each fixture warms both
functions and interleaves eight A/B/B/A blocks. Each timing sample averages four
calls for large fixtures or 64 for small fixtures. Style-call counting happens
separately: timed calls use Chrome's original API without instrumentation.

These are local headless Chrome measurements on Windows and Linux under WSL.
They exclude CDP transport, screenshots, network access and model reasoning.
They establish neither whole-agent speedups nor a production memory reduction.
Values compare baseline and candidate within each platform, not the platforms
against one another.

| Fixture | Windows baseline → candidate | Linux baseline → candidate | Style reads baseline → candidate |
| --- | ---: | ---: | ---: |
| Small form | 0.1734 → 0.1531 ms | 0.1914 → 0.1609 ms | 104 → 42 |
| 2,500-item catalogue | 30.6375 → 31.1750 ms | 17.6000 → 17.1125 ms | 3,078 → 2,673 |
| 1,000 text leaves under 64 wrappers | 36.6750 → 6.3375 ms | 20.6875 → 3.9000 ms | 68,000 → 2,066 |
| Native modal | 0.0719 → 0.0680 ms | 0.0398 → 0.0375 ms | 29 → 16 |
| Transparent ancestor | 0.0250 → 0.0258 ms | 0.0156 → 0.0156 ms | 13 → 11 |

The nested case is approximately 83% faster on Windows and 81% faster on Linux.
The Windows catalogue is 1.8% slower in this run, and the tiny transparent case
is 0.0008 ms slower. The optimization targets repeated ancestry work; it does not
claim every page becomes faster. Earlier instrumented exploratory runs are not
used in this timing table.

## Correctness and qualification

Both platforms produced exactly the baseline observation for all five timed
fixtures, four changing-style states, seven depth-boundary cases and a fixture
that exceeds the 8,192-entry cache capacity. The depth fixture visits an
over-depth leaf before a readable shorter suffix; this prevents a truncated walk
from poisoning valid cached ancestors. Opacity and display changes between calls
must be reflected immediately. A separate capacity fixture reduces style reads
from 13,500 to 10,502 while retaining the same text.

The compiled runtime at `48a6facba8fd9105cc90c79f654d389d99678ab3` passed all 12
native browser flows on Windows and Linux, including screenshots, guarded
clicks, independent subsequent observations and cleanup of owned profiles. The
new fixture crosses both the depth and capacity limits, toggles ancestor opacity
through the normal input path and verifies fresh visibility twice. Core and
browser-protocol suites also passed on both platforms. The existing hosted
Chromium qualification invokes these flows through `--chromium` and retains
the headed and compatibility checks. Hosted qualification is a separate gate.

To reproduce with a local Chrome installation and the baseline Git object:

```sh
node cpp-mcp/scripts/benchmark-browser-styles.mjs fdaa79d47f24ce1cb44d28372d6e6971b81bc7e2
```

Set `DEVBOX_TEST_CHROME` when Chrome is outside the standard paths. This is a
development probe using an isolated temporary profile and local fixture; Node
is not a dependency of the C++23 runtime.
