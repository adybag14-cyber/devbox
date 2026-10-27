# Memory sampling after capture workspace release

The event-loop monitor copies its bounded histories and allocates percentile
scratch space during each capture. Previously, those vectors were still alive
when it sampled process memory and the global C++ allocation counters. A growing
history could therefore inflate the reported current requested bytes even when
the working storage was released immediately after the capture returned.

The C++23 capture now finishes the event-loop JSON in an inner scope, releasing
the history copies and scratch before collecting process and allocator metrics.
The sampling cadence, source histories, percentile calculations, lock boundaries,
JSON fields and cumulative/peak allocation accounting are unchanged. This is a
measurement correction, not evidence of eliminating a retained leak. An OS
allocator can retain freed pages, so the change does not promise lower RSS.

`performance-capture-memory` exercises the actual 20 ms sampler for at least
1,500 samples, stops and joins the sampler and persistence workers, then captures
the frozen history. It compares the reported requested bytes with the independent,
allocation-free counter, allowing one extra snapshot-copy-sized JSON temporary.
The old implementation fails because the larger history workspace exceeds that
bound. Sanitizer builds still exercise capture and frozen-history consistency;
their disabled custom allocation counter cannot validate the byte comparison.

On a local Linux GCC 12 Release build on 27 October 2026:

| Quiescent sampler fixture | Before | After |
|---|---:|---:|
| Retained samples at capture | 1,519 | 1,525 |
| Reported requested bytes | 361,477 | 324,541 |
| Independent live bytes including snapshot copy | 323,857 | 323,857 |
| Snapshot-copy allocation bytes | 2,702 | 2,702 |
| Reported excess | 37,620 | 684 |

The six-sample difference reflects real timer scheduling; this is a regression
reproducer, not a matched CPU benchmark. The remaining small excess is within
the declared JSON-construction tolerance. The existing real-executor-stall,
history-window and full-sort percentile-reference checks also passed.

Run with a normal native test build:

```sh
ctest --test-dir BUILD -R '^(telemetry|performance-window|performance-statistics|performance-capture-memory)$' --output-on-failure
```

This follow-up is separate from the deployed bootstrap-v0.9.12 release. Local
tests alone do not establish hosted qualification, publication or deployment.
