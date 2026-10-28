# Native readiness allocation reduction

The readiness endpoint needs a fresh scheduler snapshot and a running, successful
execution-store probe. Previously it copied the complete cached scheduler payload,
constructed diagnostic JSON for every background worker, and copied the store-health
JSON to inspect a few fields. The C++23 implementation now reads those cached fields
under their existing mutex and checks the selected worker's typed state directly.
The monitor and worker locks remain separate. The 5-second scheduler and 150-second
store/worker freshness limits, complete-tool-count requirement, failure handling,
and diagnostic snapshot format are unchanged.

Timestamp age calculation also reads the existing string by reference and samples
the wall clock once. Missing or malformed timestamps remain unavailable; future
timestamps still clamp to zero age and pre-epoch timestamps retain their established
handling. No runtime dependency or MCP action is added.

## Measurement

On 28 October 2026, three fresh process trials per variant ran on the same Windows
workstation with alternating order. Both used MSVC 19.51 Release builds with IPO and
the same dependency SDK. Each trial required a responding native state coordinator,
waited 20 seconds, issued 200 loopback readiness requests followed by 200 metadata
requests, waited another 20 seconds, and verified native coordinator shutdown.
Every trial used separate private state. No production state or service was changed.

The baseline source tree was `5c43d41c23295a64d16a68e87db58f551c3ea149`, identical to
release source `801c24f7cccc007cf4dc914e1ebb11ad77181475`. The candidate code commit was
`196f9907a040d45acc996e57b955f6c12001c5d7`. Each executable's embedded source identity
and SHA-256 were checked. An earlier binary with pre-commit identity was rejected
and excluded before these six qualified trials.

Medians across the three trials per variant:

| Phase / metric | Baseline | Candidate | Change |
| --- | ---: | ---: | ---: |
| 200 readiness requests: C++ `operator new` calls | 158,477 | 101,047 | 36.2% fewer |
| 200 readiness requests: C++ requested bytes | 15,325,227 | 9,519,847 | 37.9% fewer |
| 200 readiness requests: elapsed time | 249.18 ms | 237.57 ms | 4.7% lower in this sample |
| 200 metadata requests: C++ `operator new` calls | 254,943 | 254,037 | 0.4% fewer |
| 200 metadata requests: C++ requested bytes | 25,677,703 | 25,692,123 | 0.1% more |
| 200 metadata requests: elapsed time | 256.85 ms | 242.37 ms | 5.6% lower in this sample |

The allocation saving is the supported result. The unchanged metadata control also
became faster by a similar amount, so this experiment does not establish a causal
elapsed-time improvement. The workstation was shared, the sample is small, and the
counters include the benchmark's native HTTP client and periodic background work.
They cover the standard C++ `operator new` forms, including arrays and aligned
allocations. Direct `malloc` calls from C libraries and allocator padding/headers
are excluded; these percentages are not savings across all process allocations.
These are not end-to-end ChatGPT/browser timings or a long-run leak-freedom result.
All individual readiness/control measurements are in
[the measurement record](benchmarks/readiness-allocation-20261028.json).
Every trial's emitted final `ready` value and resource snapshot is retained there;
all six were ready with zero connections. The qualification controller checked
those values as well as coordinator shutdown. The benchmark itself now also exits
unsuccessfully if it finishes unready or with active connections.

## Validation and reproduction

The `engine`, `background-state`, `telemetry`, and `core` suites passed on Windows
and Linux/GCC 12.3. Regression coverage includes missing, stopped, failed, expired,
and recovered workers; bounded string views; concurrent failures; timestamp edge
cases; and readiness after real scheduler/storage probes. A thousand typed worker
health queries allocate no C++ diagnostic objects.

Configure with `DEVBOX_BUILD_TESTS=ON` and `DEVBOX_BUILD_BENCHMARKS=ON`, then build
`devbox-server-lifetime-bench` and the relevant test targets. Run the benchmark with
a **new absolute fixture directory** as its sole argument. Repeat with unchanged
compiler/SDK settings at the baseline and candidate revisions, alternating variants.
Use the phase JSON records for comparison and require the final healthy resource
snapshot and successful native coordinator cleanup. A wrapper that prohibits the
coordinator's separate process lifetime makes the fixture invalid; do not accept a
degraded run or relax its checks to obtain a timing number.

The original host was an AMD Ryzen 9 7950X3D (16 cores, 32 logical processors),
Windows 11 Pro x64 build 26300, with 67,736,338,432 bytes of OS-visible physical
memory. It was a shared workstation, not an exclusive benchmark machine.
The shared `x64-windows-static` SDK used vcpkg commit
`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`; its installed-status file SHA-256 was
`da74397ce9a646610d990e1f1616279a9d51850fe57d24ea3aaa865f18d99c3e`.
Relevant packages were Boost.Asio 1.92.0#1, Boost.Beast 1.92.0, curl 8.22.0,
Lexbor 3.0.0, nlohmann-json 3.12.0#2, OpenSSL 3.6.4, pugixml 1.16,
SQLite 3.53.4, and zlib 1.3.2#2. Both variants used that same prebuilt SDK with
installation disabled.

The effective configure/build commands were the following, with `$vcpkg` naming
that vcpkg checkout and `$sdk` the matching installed SDK directory:

```powershell
cmake -S . -B .cpp-build/windows -G "Visual Studio 18 2026" -A x64 "-DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake" "-DVCPKG_INSTALLED_DIR=$sdk" -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_TARGET_TRIPLET=x64-windows-static -DDEVBOX_BUILD_TESTS=ON -DDEVBOX_BUILD_BENCHMARKS=ON -DDEVBOX_BUILD_TUI=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON
cmake --build .cpp-build/windows --config Release --target devbox-server-lifetime-bench --parallel 3
& ./.cpp-build/windows/cpp-mcp/Release/devbox-server-lifetime-bench.exe C:/Bench/new-readiness-fixture
```

Use a new fixture path for every invocation. The measured order was baseline,
candidate, candidate, baseline, baseline, candidate. Each process performs its
own fixed 20-second idle periods and 200-request phases; do not run a build or
another benchmark concurrently with those six trials.
