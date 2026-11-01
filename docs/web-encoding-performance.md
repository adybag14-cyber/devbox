# Web document encoding validation

UTF-8 web responses previously passed through `Json(std::string(input)).dump()`
only to validate their encoding, then acquired another complete body copy before
extraction. Large pages paid for JSON escaping and temporary buffers even when
the extracted answer retained only a small excerpt.

The C++23 path now checks the complete response with an allocation-free UTF-8
validator and borrows its original body during synchronous extraction. The
validator follows the byte ranges in [RFC 3629 section 4](https://www.rfc-editor.org/rfc/rfc3629.html#section-4),
rejecting overlong encodings, surrogate code points, truncated sequences and
values above U+10FFFF. Legacy Windows-1252/ISO-8859-1 handling retains its existing
conversion buffer. Unsupported declared encodings and malformed UTF-8 still
return `unsupported_encoding`; no text is silently repaired into evidence.

The original response remains alive throughout extraction. Its SHA-256 still
describes the fetched bytes, and content hashing describes the extracted text.
Encoding validation scans beyond the excerpt boundary, so a malformed suffix
cannot become valid merely because the visible answer is clipped. This change
adds no runtime dependency, permission, network request or browser capability.

## Correctness evidence

Windows MSVC and Linux GCC passed the encoding, web-research and offer tests.
The dedicated regression covers all 1,112,064 Unicode scalar values, 85,792
comparisons against the previous JSON validator, malformed scalar boundaries,
original-byte provenance, an invalid suffix beyond the excerpt limit, legacy
currency conversion and unsupported encodings. The full-buffer validator made
zero allocation calls. The parser fuzzer also compares its decision against the
retained JSON validator for every mutated input.

## Measurement method

`devbox-web-encoding-bench` measures complete `extract_document` calls with
ASCII text, multilingual text, a Windows-1252 control and HTML containing a large
script. Input sizes are approximately 128 KiB, 1 MiB and 4 MiB. Input creation,
output equality checking and reporting stay outside the measured extraction.
Each process runs five trials of ten extractions for every case.

The saved baseline and candidate binaries run in A/B/B/A order on each platform,
serially after local builds finish. Source identities, output digests and every
trial are retained in the companion benchmark evidence. Native allocation
counters measure C++ allocation requests, including freed temporaries; they do
not measure peak live RAM or all direct C allocations. Shared-host timing and
synthetic pages limit generalization. These figures exclude network transfer,
site delays, browser rendering and ChatGPT reasoning.

## Results

Baseline source `709c58c6e75025d772c0e184063e71a0998a4002` and candidate
`70d835af5fec821ac5bd9f7b0ccc570a28a3070d` produced matching output digests in
every case. The [full evidence](benchmarks/web-encoding-20261031.json) retains
all eight process runs and their individual trials.

| 4 MiB case | Platform | Baseline ms / 10 | Candidate ms / 10 | Time reduction | Requested-byte reduction |
| --- | --- | ---: | ---: | ---: | ---: |
| HTML with a large script | Windows | 174.26 | 79.35 | 54.5% | 97.7% |
| HTML with a large script | Linux | 126.11 | 56.13 | 55.5% | 97.8% |
| Multilingual text | Windows | 126.75 | 41.98 | 66.9% | 97.8% |
| Multilingual text | Linux | 98.45 | 38.16 | 61.2% | 97.6% |

The unchanged legacy-encoding control allocated exactly the same bytes before
and after. Its timing varied, including a 1.5% slowdown in the Linux 4 MiB case;
no improvement is claimed for that control. Across UTF-8 fixture sizes and
platforms, observed extraction-time reductions ranged from 29.2% to 80.6%.
This is not evidence of the same percentage improvement in an end-to-end
research answer or production peak memory.
