# Native dependency resilience

Bundle 0.8.0 reduces external failure points and unnecessary compilation work while
retaining maintained networking, cryptography, storage and parsing libraries.
Full and core release archives remain available; publishing does not migrate a
running installation.

## Implemented reductions

- Local Linux X11 capture (`DISPLAY=:N`) reuses Devbox's C++ XCB window/pixel backend.
  Program, process-tree and display capture work with an empty runtime PATH and
  XTEST disabled. They do not need xdotool, wmctrl, xwininfo or ImageMagick.
  The read-only path does not change focus or inject input. Computer-use actions
  retain their existing XTEST, opt-in, ownership and stale-observation checks.
- Boost's umbrella includes were replaced with the headers actually required by
  production code. Components and tests now declare their own coroutine/context
  includes instead of relying on unrelated transitive includes. Boost remains the
  maintained network implementation; this is a build-footprint reduction.
- Research query expressions are compiled once per job and reused for document
  and citation matching. Main-content extraction and bounded diagnostic records
  reduce repeated irrelevant evidence rather than padding source targets.

## Measured tradeoffs

The following are local component measurements using GCC 12, Release flags and
three sequential preprocessing samples from the actual Ninja compile database.
They do not establish an end-to-end build or production latency improvement.

| Translation unit | Preprocessed bytes before | After | Boost headers before / after | Median preprocessing CPU seconds before / after |
|---|---:|---:|---:|---:|
| `async.cpp` | 7,081,194 | 5,474,828 | 481 / 237 | 0.350 / 0.223 |
| `server.cpp` | 9,806,932 | 7,988,063 | 1,068 / 783 | 0.548 / 0.409 |

The repository-owned `devbox-compression-bench` compares representative JSON
payloads with alternating backend order, three warm-ups and 25 samples. Allocation
and the production-style 50-percent acceptance policy are included. Accepted
gzip results are decoded independently and checked byte-for-byte.

| Payload | libdeflate median ms | zlib streaming median ms | libdeflate / zlib output bytes |
|---|---:|---:|---:|
| ASCII, 128 KiB | 0.106 | 0.107 | 4,789 / 4,876 |
| Unicode, 512 KiB | 0.383 | 0.412 | 12,504 / 13,221 |
| ASCII, 1 MiB | 0.776 | 0.923 | 37,163 / 38,734 |
| Poorly compressible, 512 KiB | 0.015 | 0.054 | both rejected by the size policy |

The measured zlib-only alternative increased CPU work on these samples, including
about 19% more compression time for the one-MiB case and a slower rejection probe.
Both libraries are therefore retained: libdeflate for the bounded fast path and
zlib for incremental, cancellable larger responses and native PNG encoding.

Build the benchmark with `DEVBOX_BUILD_BENCHMARKS=ON` (or the native test build),
then run `devbox-compression-bench`. `--verify` performs the round-trip checks
without collecting a performance sample distribution. Hosted qualification runs
the verification case; results depend on compiler, CPU and workload.

## Retained boundaries

- Keep libcurl/OpenSSL, SQLite, Lexbor/pugixml, Ada URL, RE2/Abseil and JSON parsing.
  They already execute as native code; replacing them transfers compatibility,
  security and recovery responsibilities to Devbox.
- Keep GitHub CLI for signed installation/promotion and the optional GitHub
  credential bridge. A replacement must reproduce certificate identity, trust-root,
  transparency and exact source/workflow restrictions before activation. No trust
  check or assertion was removed to lower the dependency count.
- Browsers, displays, tunnel software and external websites remain capability or
  service dependencies. Native wrappers cannot eliminate provider outages.
- macOS/Wayland/remote-display capture compatibility helpers remain. The tested
  native consolidation specifically covers the canonical local X11 path.

Core startup still needs only the qualified native executable and supported OS
facilities/TLS trust. Optional GUI or GitHub functionality has its own requirements.
