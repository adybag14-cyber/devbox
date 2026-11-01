# Account for the bytes actually stored in the web cache

The web cache checked a compact JSON encoding against its 512 KiB entry limit
and 64 MiB total quota, then serialized the same document again with indentation
for storage. Those larger written bytes could exceed the admitted budget.

The C++23 cache now serializes once with Devbox's existing compact JSON encoder
and atomically writes that exact buffer. Entry admission, total quota accounting
and the stored representation therefore agree. The common byte writer retains
the previous temporary-file naming, flush, replacement and exception cleanup.
Other JSON state continues using the existing two-space formatted representation.
Eviction candidates are sorted only when the incoming entry actually needs space;
oldest-file selection under quota pressure is unchanged.

## Reproduction and validation

The regression uses a real loopback HTTP source and a separate disposable cache.
After the first fetch, a size-only filler places the cache between the compact
and formatted admission boundaries. A subsequent HTTP 304 revalidation reproduces
the defect at source `c141777dd92a76ece1613b8d3ac4aa818ac28aa4` on Windows and Linux:
the files total 67,109,049 bytes, exceeding the 67,108,864-byte quota by 185 bytes.
This is the specific reproduced fixture excess, not an estimate of production
disk usage or the largest possible formatting overhead.

The corrected fixture requires the actual total to remain within quota, the
stored bytes to equal their compact encoding, and the unrelated fitting entry to
remain. It then verifies a fresh cache hit and forces genuine quota pressure,
which must evict the oldest filler while preserving the refreshed source.
The core atomic-write test alternates compact and formatted state replacements
under concurrent readers; existing JSON formatting and long-path checks remain.

Build the core, research and offer test targets and run CTest with
`-R '^(core|web-research|web-cache-bytes|web-offers)$'`. The cache test creates
private temporary state only; it never points at the production cache. Removing
the second serialization and unnecessary under-quota sorting avoids repeated work;
no end-to-end timing improvement is claimed without a measured comparison.

All four suites passed on Windows MSVC and Linux GCC at corrected source
`83ffe1a48f33dc821a562b44d6a3d9f83adfc79b` on 1 November 2026. The recorded
suite times were 16.20 seconds on Windows and 15.26 seconds on Linux. These are
verification durations, not performance comparison results.
