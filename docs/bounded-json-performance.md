# Bounded response encoding

Large native MCP responses take the budgeted encoding path on the codec pool.
That path previously forwarded each write through `ostream` and `streambuf`
before reaching the bounded string. It now supplies a direct output adapter to
the same pinned JSON serializer. Each write still checks cancellation and the
exact output-byte limit, with the same Unicode replacement and number formatting.
This adds no runtime dependency and retains the aggregate response reservation.

Windows and Linux tests cover exact-size and one-byte-short capacities, zero
capacity for scalars/containers, cancellation of both short and large encodings,
large payloads, binary JSON, Unicode, invalid UTF-8 and numeric boundaries.

An eight-process comparison reused the retained Windows binaries at baseline
`2f86268b829af09cf8891e9ea42d0f4f178b799d` and candidate
`5fb0feeb0bfdf084d5e4223076f1dc7a3da77c15`. Source order was A/B/B/A twice and
detailed/compact action order was balanced. Each process performed 100 actual
loopback MCP requests per action. All eight finished ready, with zero live HTTP
connections and the exact coordinator process instance verified exited.

| Per 100 requests | Baseline median | Candidate median | Observed change |
| --- | ---: | ---: | ---: |
| Detailed status | 510.46 ms | 498.10 ms | 2.4% lower |
| Compact status | 195.90 ms | 181.84 ms | 7.2% lower |

Timing distributions overlap on this shared workstation. These are measured
local medians, not a guaranteed latency improvement, statistical significance
claim, or model/network timing. Allocation counts and requested bytes were
essentially unchanged; the intended improvement is reduced forwarding work.
The complete source identities, binary hashes, samples and cleanup receipts are
in [the evidence record](benchmarks/bounded-json-20261031.json).

The later integration candidate also repairs the optional JavaScript rollback
backend's job-state writes. That does not change this C++ benchmark's inputs;
the measurements remain explicitly bound to the original binaries above.
