# Chrome response payload ownership

The native Windows Chrome adapter now moves the helper's captured stdout into
its parser, releases that raw buffer after the JSON owns its parsed strings,
and moves the parsed image string into the MCP image content block. It retains
the executable identity check, response limits, MIME policy, explicit failure
handling and prohibition on automatically replaying uncertain input.

The measurements below describe the earlier 0.9.16 feature candidate. Its changes
are included in the [0.9.21 integration candidate](native-0921-integration.md)
against merged 0.9.15. No new browser permission or runtime dependency is introduced.

The pure response adapter is tested on Windows and Linux with JPEG/PNG envelopes,
a one-MiB encoded payload, ordinary status responses, grant denial, malformed
JSON, invalid success fields, unsupported MIME types, and missing, empty or
non-string image payloads. Tests compare the
established result envelope and verify that image data is not duplicated into
structured metadata. Invalid replies retain `outcome: unknown` and
`automatic_input_replay_allowed: false`.

Baseline source `1ab8027e7805a264314e4be74478a00e89fcf297` preserves the original
formatting algorithm after a test portability correction. Candidate source
`d117aa21268d18e577ee1d55f01ad4343fafc0ab` contains the ownership changes.
Five trials at each payload size performed 20 adapters per trial. The prepared
input was outside the timed/counted region; JSON parsing and MCP envelope
construction were included. Exact source, compiler and binary identities plus
all trials are in [the evidence record](benchmarks/chrome-payload-20261031.json).

| Platform / encoded image | Requested bytes before → after, per 20 | Median time before → after |
| --- | ---: | ---: |
| Windows / 32 KiB | 7,317,480 → 6,661,020 | 3.895 → 3.430 ms |
| Windows / 512 KiB | 120,422,140 → 109,935,280 | 53.766 → 50.381 ms |
| Windows / 4 MiB | 926,152,360 → 842,265,180 | 347.213 → 364.099 ms |
| Linux / 32 KiB | 7,178,880 → 6,523,500 | 3.956 → 4.764 ms |
| Linux / 512 KiB | 112,855,760 → 102,369,980 | 73.397 → 77.069 ms |
| Linux / 4 MiB | 901,909,260 → 818,023,160 | 833.831 → 657.544 ms |

One image-sized allocation per response was removed, reducing measured requested
bytes by 8.7–9.3%. Payload hashes and returned byte counts matched. Timing was
mixed on this shared workstation, so this is not a universal latency or peak-RAM
claim. The additional stdout transfer improvement is outside this adapter-only
measurement. Real screenshot capture, subprocess startup, network and model time
are excluded. The Linux measurement exercises the portable response formatter;
it does not make the Windows Chrome integration available on Linux.
