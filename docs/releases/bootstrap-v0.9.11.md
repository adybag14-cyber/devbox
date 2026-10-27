# Devbox C++23 0.9.11

The performance monitor now retains its bounded delay and drift histories in
contiguous C++23 buffers. Sampling cadence, chronological order, expiry boundaries
and published percentile fields remain unchanged. On the measured Windows MSVC
build, retained history storage fell by 160,720 bytes and ingestion allocations
fell from 16,919 to four across the accelerated one-hour component trace. The
buffers reserve about 239 KiB at construction. Capture timing ranges overlap;
these results do not establish a whole-server response-time improvement. See
[the component measurements and correctness checks](../performance-telemetry-window.md).

Operators can set `DEVBOX_BROWSER_SOFTWARE_RENDERING=1` for Devbox's owned
on-demand Chromium sessions. This adds `--disable-gpu`, matching the existing
managed desktop browser policy on virtual displays. The default remains off.
The option preserves JavaScript, HTML capture, screenshots and input; it does
not change sandboxing, network restrictions, session limits or deadlines. See
[software rendering configuration and qualification](../browser-software-rendering.md).

The real-browser qualification checks the requested Chromium argument, rendered
prices, HTML, screenshots, scrolling and input. It also verifies that ordinary
headless sessions omit the flag by default, independent of an inherited operator
environment. GPU-dependent content still requires deployment-specific testing.

This release continues the native server, supervisor, desktop broker, installer
and setup packaging from [0.9.10](bootstrap-v0.9.10.md). Full desktop and offline
packages retain the verified optional ripgrep fallback; separate core archives
omit it. Windows user and portable installs do not require administrator rights.
Node, Python, a Docker engine, Chromium and a Cloudflare tunnel are not bundled.

Publishing this release does not change an existing deployment's browser profile
or upgrade its running processes. Each production promotion must verify the
signed artifact and preserve active work through the native maintenance gates.
