# Windows file-worker performance qualification

File tools keep a separate, bounded C++23 worker so a blocked filesystem call can be terminated without wedging the MCP frontend. Performance work must preserve that boundary, exact bytes, atomic compare-and-swap/replay, cancellation, deadlines and resource limits.

## Removed overhead

- A filesystem worker only uses its explicit stdin/stdout/stderr pipes. It now uses the existing `windows_detached_console` option to avoid an unused console host. `DETACHED_PROCESS` changes console attachment; it is not `CREATE_BREAKAWAY_FROM_JOB`. The worker is still created suspended, assigned to its bounded Job Object and then resumed.
- Stdin advances through at most sixteen 64 KiB writes before servicing output and cancellation again. Completed final input closes immediately to deliver EOF. Pending overlapped writes wake the loop through their completion event instead of waiting for a polling tick. Both synchronous and asynchronous completion counts come from `GetOverlappedResult`.
- A bounded output drain reports whether it made progress. The loop services another bounded pass immediately after progress, while still checking cancellation and the deadline. It waits when no pipe can advance, rather than throttling available output by the Windows timer quantum.
- A zero retention limit counts sanitized Unicode scalars without constructing per-character strings/deques. Entirely ASCII chunks with no preceding incomplete sequence need neither sanitizer copies nor retained text. Malformed, multibyte and split sequences retain their previous replacement/counting behavior.

Normal user programs retain their existing console-launch mode. The tool registry, permissions, response schemas, durability operations and worker memory/process limits are unchanged. No new runtime dependency is introduced.

## Reproduce the native diagnostic

Configure the normal pinned C++ build with `DEVBOX_BUILD_BENCHMARKS=ON`, then build `devbox-filesystem-latency-bench`. Pass a **new absolute fixture directory under an existing parent**:

```powershell
cmake --build .cpp-build/windows-pinned --config Release --target devbox-filesystem-latency-bench
.\.cpp-build\windows-pinned\cpp-mcp\Release\devbox-filesystem-latency-bench.exe C:\path\to\new-fixture
```

The executable emits JSON Lines containing build identity, exact-output validation, all measured samples, child operation time and parent allocation counters. It measures:

- An empty worker round trip and zero-retention processing of 1 MiB.
- Direct and isolated 4 KiB, 32 KiB and 512 KiB reads (100 measured samples and five warmups each).
- Direct and isolated atomic writes of 4 KiB, 32 KiB, 65,537 bytes and 1 MiB (20 measured samples and five warmups each).

The direct lane is a diagnostic lower bound, not a production replacement. Cumulative allocated bytes are not resident memory. Parent allocation counters exclude allocations inside workers. Use an outer Windows Job Object when collecting CPU/peak committed memory for the complete owned process tree.

For comparisons, use identical benchmark source, compiler, dependencies, payloads and build configuration against the baseline and candidate runtime sources. Preserve both binaries and their hashes. Alternate their order and retain individual trials; do not infer a general speedup from one run or conflate local worker latency with remote connector latency.

## Correctness checks

The process suite verifies exact byte length and SHA-256 at and around pipe-buffer/write boundaries, delayed readers, timeout/cancellation while stdin is blocked, mixed malformed UTF-8 split into different chunks, and empty zero capture. Existing tests cover simultaneous large stdin/stdout/stderr, bounded capture, process-tree cleanup, filesystem deadlines, exact file bytes, atomic conflicts/replay, deep checkpoints and multi-megabyte worker responses.

The wider native suite must pass on the exact source being proposed. Native desktop tests belong in a controlled desktop/CI session; timing probes must not operate unrelated user windows.

The pending-stdin regression also exposed a macOS correctness issue: Darwin's non-socket `EPIPE` path signals the process, so blocking SIGPIPE only on the writing thread cannot protect another unblocked thread. The parent now sets `F_SETNOSIGPIPE` on its owned stdin write descriptor before launching the child. This retains `EPIPE` error reporting without changing the application's global signal disposition. The original regression remains enabled. See Apple's [write signal path](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_generic.c) and [descriptor-specific suppression](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_descrip.c).

## Interpret agent-facing timings separately

Correlate each measured client call with its tool invocation and HTTP request ID. Report server HTTP duration and the remaining client interval separately. That remaining interval includes connector, network and orchestration overhead; it is not a Cloudflare-only measurement. Group independent read operations concurrently when their semantics allow it, retaining sequential order for dependencies and serialized desktop input.

Microsoft references: [overlapped WriteFile completion](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile), [waiting for multiple handles](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects), [process creation flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags), and [Job Object containment](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).
