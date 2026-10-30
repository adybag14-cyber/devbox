# Devbox C++23 0.9.14

This release adds the optional Windows `devbox_chrome_bridge` action. It connects
the C++23 server to the independent Rust Chrome native-messaging host, preserving
structured action failures and returning screenshots as MCP image content. The
operator must configure `DEVBOX_CHROME_BRIDGE_EXE` and the exact
`DEVBOX_CHROME_BRIDGE_SHA256`. The adapter holds an executable read lock while it
checks the hash and starts the pinned image, and it sends bounded JSON without a
shell. The default is disabled.

The native supervisor now retains a committed version when an ordinary restart
fails, and retries it with bounded backoff. Previously, a readiness timeout could
silently select a historical version even after a completed deployment. Automatic
rollback remains available for an unfinished promotion, subject to image identity,
durable-idle and coordinator-stop checks. A persisted handoff that does not match
the selected previous or candidate image is rejected before starting companions;
it cannot overwrite a separately selected version. The Windows/Linux management fixture
injects a temporary selected-image lookup failure, verifies recovery on the same
version without replaying effects, and then checks explicit rollback.

The tool contract advances from 11 to 12; the durable state schema remains 2 and
the coordinator protocol remains 1. The old manager deliberately refuses a live
cross-contract promotion. A reviewed transition must drain and stop the old
manager, authenticate the new manager, use its stopped-instance promotion path,
and update service entrypoints before restarting. Keep the verified old manager
and configuration for a stopped-instance rollback. Do not bypass contract checks.

The separate Chrome bridge release asset includes its native executable,
extension, dependency licence texts and source/file manifest. The release job
requires its own tests and verifies its GitHub attestation, source and checksums
before publication. Its Windows C runtime is statically linked. The installer
registers only the current user's native host; the extension is loaded through
Chrome's normal permission review. No interpreter or browser is bundled.

The bridge exposes DOM text, accessible controls, screenshots, bounded HTML,
scrolling, keyboard/mouse input, rich-text selection, open shadow roots and
same-origin navigation. Explicit per-tab grants, fresh observations, bounded
deadlines and replay receipts govern access. Unknown input outcomes require
observation before any retry. The independent native Windows fallback provides
Chrome screenshots and input when Devbox is unavailable. Native text entry is
single-line; multiline text uses the extension. See the
[bridge guide](../../chrome-bridge/README.md) for functionality and limits.

Readiness probes now inspect the required typed worker/store health fields
without constructing full diagnostic snapshots. In the retained controlled
Windows fixture, 200 readiness probes used 36.2% fewer C++ allocation calls and
37.9% fewer requested bytes. Wall-clock improvement was inconclusive; these are
not whole-server or ChatGPT latency claims. Health freshness, locking and error
semantics are preserved. See the
[readiness measurements](../readiness-allocation-benchmark.md).

Lean/core archives, user-level Windows installs and universal offline bundles
continue from [0.9.13](bootstrap-v0.9.13.md). The Chrome bridge is an optional
Windows asset, and does not become a core or Linux/Oracle runtime dependency.
Release qualification and publication do not themselves activate production.
