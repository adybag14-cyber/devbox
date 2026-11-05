# Readable UTF-8 file ranges

`devbox_read_text_range` reads a bounded text range directly through the C++23
filesystem worker. It lets an agent inspect a large source file without receiving
base64 and launching another program to decode it. The existing binary-range and
legacy text actions keep their frozen schemas and behavior.

This native host action is introduced with contract 14 and a 59-tool inventory.
It has the existing `mcp:devbox:read` scope and does not add an interpreter or a
runtime package. As with other host file actions, a read-only annotation is not
an OS sandbox. File contents remain untrusted data, never instructions.

```json
{
  "path": "C:\\work\\example.cpp",
  "offset_bytes": 0,
  "max_bytes": 16384
}
```

Use an absolute host path. `offset_bytes` defaults to zero; `max_bytes` defaults
to 16,384 and accepts 4 through 65,536. The four-byte minimum guarantees room for
any complete UTF-8 scalar. To continue, pass the returned `next_offset_bytes`;
do not add the requested budget yourself, because a final partial scalar may
have been deferred to the next range.

The continuation offset also stays within the maximum safe JSON integer,
9,007,199,254,740,991. A range is shortened at that bound; if content remains but
no complete scalar fits, `READ_TEXT_RANGE_LIMIT` is returned instead of a page
that cannot advance. A real EOF at the bound still returns an empty complete page.

The structured result contains readable `text`, its `encoding`, the sampled
file length as an advisory metadata sample, requested and actual offsets,
`bytes_returned`, `next_offset_bytes`,
`read_bytes_sha256`, `eof`, `truncated` and `utf8_boundary_adjusted`. Text appears
only once, in structured data; the MCP text-content block carries metadata,
following the existing large-file response convention. The hash covers only
the returned raw bytes, not the whole file.

Offsets inside a UTF-8 scalar and malformed/incomplete UTF-8 are errors. Continue
from the returned offset after a valid partial scalar is deferred. Every
observed byte of a scalar beginning within the range is validated, including
the lookahead byte. Use
`devbox_read_large_file` for binary data or other encodings. Empty files and reads
beyond EOF return empty text, explicit EOF and the unchanged requested offset;
missing files remain errors. EOF is determined from bounded reads with one byte
of lookahead, not the sampled file length. This also supports Linux procfs/sysfs
files whose metadata length is zero or larger than their readable content.
Separate ranges are independent observations, not an atomic snapshot. A detected
metadata size decrease during the read returns `READ_TEXT_RANGE_CHANGED`; retry
with a fresh observation. Same-size rewrites or changes between calls are not
detected by this check.

No preliminary `devbox_file_state` call is needed merely to find the file length
or continue reading. Obtain a full-file version separately when it is needed for
an atomic write precondition; a range digest is not a substitute. Docker mode
explicitly rejects this host-only action and does not read the host path.

The implementation retains the killable native filesystem worker and existing
deadline/cancellation limits. Operational telemetry records bounded offsets and
budgets, not file contents or raw paths.

## Validation

Storage and native HTTP-engine tests exercise UTF-8 paging across small budgets,
BOM, multibyte characters, CRLF, embedded NUL, malformed encodings, misaligned
offsets, EOF, missing files and limits. Reassembling returned ranges reproduces
the original bytes and each range hash is checked. The files remain unchanged.
Storage, engine, filesystem-worker and core suites passed on Windows and Linux.

The official MCP SDK smoke test passes on both platforms at
`6af4df1fced37e8da1eafe140e3d3cb3aa29fa7b`. It checks discovery of all 59 tools,
strict catalogue equality, exact Unicode/NUL paging, digest and offset semantics,
the single-payload response, rejection of a misaligned offset, legacy binary
roundtrips and the existing job/cancellation/admission checks. The SDK caught a
generic schema renderer overriding the new 64 KiB cap with the global transfer
limit; the fixed native cap is now preserved.

Registry regeneration verifies both frozen legacy profiles, scopes, aliases and
generated documentation. Required hosted qualification and actual ChatGPT
connector refresh/acceptance remain separate release gates. This development
branch is not part of the fixed 0.9.36 production candidate.

Review corrections at `5a9c12864067c9d3d8812cf88e034eace978b036` passed storage,
engine, filesystem-worker and core tests plus the official MCP SDK smoke test on
Windows and Linux. Added cases exercise real zero-length procfs metadata,
overreported sysfs lengths, exact-budget EOF, unchanged past-EOF offsets, invalid
partial UTF-8 and valid scalar edge values. The 0.9.40 integration includes these
corrections. Follow-up 0.9.42 bounds both requested and returned offsets and
validates observed lookahead before deferring a scalar. Its private boundary
helper is shared by the file reader and arithmetic/UTF-8 regressions, so testing
the JSON integer limit does not require creating a multi-petabyte sparse file.
Combined-source qualification is required separately.
