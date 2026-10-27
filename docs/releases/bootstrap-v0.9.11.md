# Devbox C++23 0.9.11

The performance monitor now retains its bounded delay and drift histories in
contiguous C++23 buffers. Sampling cadence, chronological order, expiry boundaries
and published percentile fields remain unchanged. On the measured Windows MSVC
build, retained history storage fell by 160,720 bytes and allocation calls for
ingestion plus the final checksum snapshot fell from 16,919 to four across the
accelerated one-hour component trace. The buffers reserve about 239 KiB at
construction. Capture timing ranges overlap;
these results do not establish a whole-server response-time improvement. See
[the component measurements and correctness checks](../performance-telemetry-window.md).

Snapshot calculations also reuse one scratch buffer across the three windows and
select the exact percentile ranks without fully sorting every sample. A bounded
frequency table handles windows with at most eight distinct values. In the
accelerated one-hour capture traces at the steady ten-second persistence cadence,
allocation calls fell from 23,265 to 1,082. Outputs match a full-sort reference;
component timings and their limits are documented in
[the percentile comparison](../performance-percentiles.md).

Browser `open` results now separate startup into browser/proxy preparation,
endpoint readiness, debugger connection and session setup durations. Navigation
and its continuation calls report cumulative submission and document-wait time,
excluding the agent's thinking time between calls. Existing screenshot, HTML and
observation timings remain available. These native monotonic-clock measurements
add no browser commands or changed deadlines. See
[the browser phase timing fields](../browser-phase-timing.md).

Operators can set `DEVBOX_BROWSER_SOFTWARE_RENDERING=1` for Devbox's owned
on-demand Chromium sessions. This adds `--disable-gpu`, matching the existing
managed desktop browser policy on virtual displays. The default remains off.
The option preserves JavaScript, HTML capture, screenshots and input; it does
not change sandboxing, network restrictions, session limits or deadlines. See
[software rendering configuration and qualification](../browser-software-rendering.md).

The real-browser qualification checks the requested Chromium argument, rendered
prices, HTML, screenshots, scrolling and input. It also verifies that ordinary
headless sessions omit the flag when `DEVBOX_BROWSER_SOFTWARE_RENDERING` is unset.
The fixture clears and restores any inherited setting to test that default.
GPU-dependent content still requires deployment-specific testing.

The browser qualification fixture now handles bounded concurrent HTTP connections
on one I/O thread. Idle preconnections and deliberately slow fixture resources no
longer block ready requests. Transient accept errors retry, and teardown cancels
pending requests and scripted delays. The existing image-count, rendering and
context-isolation assertions remain required.

The optional JavaScript and Rust reference dependencies are updated to MCP SDK
1.32.2, jose 6.2.13, rmcp 3.7.0 / macros 3.8.0, and tokio 1.54.1. These reference
dependencies are separate from the packaged C++ server and its dependency set.

This release continues the native server, supervisor, desktop broker, installer
and setup packaging from [0.9.10](bootstrap-v0.9.10.md). Full desktop and offline
packages retain the verified optional ripgrep fallback; separate core archives
omit it. Windows user and portable installs do not require administrator rights.
Node, Python, a Docker engine, Chromium and a Cloudflare tunnel are not bundled.

Publishing this release does not change an existing deployment's browser profile
or upgrade its running processes. Each production promotion must verify the
signed artifact and preserve active work through the native maintenance gates.
