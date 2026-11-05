# Native worker output transport

The C++23 filesystem worker retains its process isolation, byte and shape limits,
deadlines, cancellation acknowledgement and atomic-write preconditions. The
0.9.40 integration candidate includes the output-completion change developed
in 0.9.38, reducing result-copying and changing Windows output readiness handling.

The worker moves its result into the response envelope, and the parent moves the
parsed value out of that envelope. Large base64 strings and nested checkpoints
therefore do not need another JSON-tree copy at either boundary. Other platforms
retain their existing process transport.

On Windows, polling the process handle could leave newly available stdout or
stderr waiting for another timer tick. Each output stream now has a local,
overlapped byte pipe, one 16 KiB buffer and one completion event. The process
loop waits for output events alongside process exit and pending stdin completion.
It still drains at most 16 completions per stream before servicing cancellation
and the other stream. The five-millisecond cancellation/deadline bound remains.
Zero-byte writes do not generate empty callbacks.

Pending I/O is cancelled and acknowledged before the operation structure or
buffer leaves scope, including callback exceptions. This follows the Windows
[ReadFile completion and buffer-lifetime requirements](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)
and [pending-I/O cancellation guidance](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations).
The pipes reject remote clients, use unique names with first-instance creation,
and expose only the explicit child handles through the existing inheritance list.
No third-party runtime dependency, public action or permission is added.

## Measurements and limitations

[Complete data](benchmarks/filesystem-response-20261104.json) retains all 15,360
timed calls from the exploratory and final comparisons, source identities,
executable hashes, allocation counts, operation timings and platform details.
Each cohort runs fresh baseline/candidate/candidate/baseline processes in order.
Every case has five untimed warmups, followed by 100 calls for 4 KiB, 32 KiB and
512 KiB files, or 20 calls for 4 MiB files. Every read checks its exact base64 bytes.
In-process lanes are diagnostic controls; production continues using the worker.

The final table compares the preserved baseline at
`575ea25b86327f0b295fb1246165d1a6fac5b65d` with the implementation at
`5d53f874a1faa46dfb4e4cf36f523910585ec522`. Medians combine the two runs per
implementation within each platform. Measurements use local warm-file fixtures
on Windows and Linux under WSL on the same host. They include worker startup,
IPC, parsing and result validation, and exclude MCP HTTP, model reasoning and
remote storage. They are not whole-agent latency measurements.

| Worker read | Windows baseline → candidate | Linux baseline → candidate |
| --- | ---: | ---: |
| 4 KiB | 9.476 → 9.713 ms | 2.494 → 2.532 ms |
| 32 KiB | 9.966 → 9.886 ms | 2.920 → 2.951 ms |
| 512 KiB | 30.655 → 15.778 ms | 12.313 → 11.811 ms |
| 4 MiB | 65.846 → 60.831 ms | 85.892 → 79.378 ms |

The Windows 512 KiB case is about 49% faster; the 4 MiB case is about 8% faster
on both platforms. Small reads do not consistently improve: Windows 4 KiB is
0.237 ms slower and both small Linux cases are slightly slower in this run.
For 4 MiB reads, parent cumulative allocated bytes per call fall from 79,048,235
to 71,723,628 on Windows and from 67,136,299 to 61,543,161 on Linux. The counters
include the five warmups and are normalized by all measured-plus-warmup calls.
They are allocation traffic, not an equivalent reduction in RSS or peak memory.
The Windows job's observed peak committed memory did not decrease in the final
comparison, so no peak-memory improvement is claimed.

The earlier copy-only experiment showed a 27% Windows 4 MiB slowdown. Reusing a
read buffer reduced allocations but left mixed latency. A verified CPU-0 control
also retained mixed timing; the raw evidence is preserved. The event-driven final
implementation is a later change with a new matched comparison. Its benefits
must not be inferred from the earlier allocation reductions or from comparisons
across those different measurement windows.

## Validation

The event-driven implementation at `fad851322f5b0d306994d028f8c92c0cade8244c`
passed all 74 Windows CTest cases, including native computer use/broker paths,
process cleanup, memory, management and filesystem checks. The final source adds
only the zero-byte completion guard and its regression assertion. On that final
source, filesystem-worker, storage, process, runtime and resource-budget suites
pass on Windows and Linux. The Windows real-Chromium suite also passes, including
navigation and capture continuation, proxy/cookie/storage isolation, the
65-second model-thinking pause, session expiry and owned-profile cleanup.

The new stream fixture verifies exact binary fragments around the pipe-buffer
boundaries, delayed output and stdout continuing after stderr closes. Existing
cases cover simultaneous large stdin/stdout/stderr, pending-write cancellation,
callback exceptions, child/grandchild termination, exact multi-megabyte reads,
write conflicts and idempotent replay. This transport change does not change the
public contract. The separate text-range action in the 0.9.40 integration advances
the combined contract to version 14 with 59 tools.
Required hosted qualification and deployment acceptance remain separate gates.

The first hosted Windows analysis run subsequently found excessive automatic
stack storage (C6262) and an unproved wait-array bound (C28020). The correction at
`1cb70a1c62cfb2386ace9e364c578e3370ebeed0` allocates each 16 KiB reader buffer once
on the heap and checks wait-array capacity before insertion. It does not suppress
analysis findings or relax a gate. Focused MSVC compilation with analysis and
ASAN options produces retained SARIF with zero findings, and the five affected
Windows runtime suites pass. Full hosted qualification remains required.
The timing table above is explicitly bound to the earlier measured source;
remeasure the corrected shipping build before treating those numbers as its
exact performance. The lifecycle and bounded-I/O design remain the same.

Build the existing `devbox-filesystem-latency-bench` with
`DEVBOX_BUILD_BENCHMARKS=ON`, then run it with a new absolute fixture directory and
`--read-only`. This creates only its own fixture files and never targets production.
