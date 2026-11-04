# Avoid a second Windows volume path traversal

Private state validation checks every parent for missing/reparse paths, confirms
a fixed local volume, and checks the directory and its current DACL. The Windows
volume query then traversed the same deep parent path again. An isolated diagnosis
measured this repeated lookup as a substantial part of the guard's cost.

The C++23 guard now remembers the root reached by the completed parent validation
and passes that root to the live volume query. It still calls the volume and
drive-type APIs on every validation. Parent checks, directory reparse rejection,
current process-token inspection and fresh ACL inspection remain. No validated
permissions or path result is cached. Extended DOS drive roots retain their
existing handling; non-Windows validation is unchanged.

Microsoft documents that [GetVolumePathNameW follows junctions and mounted folders](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumepathnamew).
Devbox rejects those intervening reparse points before resolving the checked root.
[GetDriveTypeW accepts a drive root with a trailing separator](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypew);
the resolved root still must report `DRIVE_FIXED`.

## Validation

Both the baseline and candidate passed private-directory, state-store and native
management tests on Windows MSVC and Linux GCC. Added Windows cases cover ordinary
and extended DOS paths, paths longer than 300 characters, relative/UNC/device path
rejection, a target junction and a junction parent, and an ACL widened after a
successful validation. The widened ACL is rejected on the next call, then the
restored private ACL succeeds. Rejected traversal must not create a child through
the junction. Fixtures are private and disposable; no real volume mount changes
or production directory mutations are performed.

## Matched native measurements

Baseline `0309aab3ba414bab15fe853cf9d04a2245ef1a19` and candidate
`e15c6857f3ba5c5fa1a572febe0184a3747b4e56` ran the existing idle-components benchmark
in six fresh Windows processes in A/B/B/A/A/B order, after builds and tests. Each
used a new private fixture path of the same length and three warmup operations
per component. Medians across the three processes per source were:

| Component | Calls | Before ms | After ms | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| Private directory validation | 100 | 160.95 | 35.03 | 78.24% |
| Admission refresh and snapshot | 100 | 194.50 | 51.53 | 73.50% |
| Indexed scheduler snapshot control | 100 | 68.27 | 68.46 | -0.27% |
| Small JSON read control | 200 | 7.93 | 8.13 | -2.49% |

[All measurements, source identities and executable hashes](benchmarks/private-directory-volume-20261101.json)
retain the remaining controls and variations. Each measured component ended with
zero retained C++ allocation growth, and every process verified its owned state
coordinator had stopped. These are component timings on a shared workstation,
not a measured reduction in production CPU or whole-agent latency. C++ counters
exclude direct C allocations and do not represent peak RAM.
