# Native search and rendered-page reading

`devbox_web_search` discovers links without starting a long research job. It keeps titles, snippets, query/rank provenance, domain filters and provider failure details. Search metadata is not consulted source evidence. The existing standard 100-source and fast 50-source research workflows remain available.

`devbox_browser` is an optional C++23 controller for an installed Chromium browser. It adds no hosted browser, model, API-key or paid-proxy dependency. The core server can still run without Chromium; `action: status` reports availability. Set the operator-controlled `DEVBOX_BROWSER_EXECUTABLE` to an absolute executable path when automatic discovery is unsuitable.

## Agent workflow

1. Use native search for discovery and `devbox_web_fetch` for inexpensive source retrieval. Use `max_age_seconds: 0` and `view: offers` for a fresh product check.
2. If useful content requires JavaScript or page interaction, call `devbox_browser` with `action: open`, the public URL, and optionally `include_html: true`.
3. Inspect `page_status`, URL, title, visible text, viewport text, controls, scroll regions and image. An access-denied page or consent screen is an intermediate outcome, not a successful source read.
4. Use the returned `session_id` and latest `observation_id` for scroll/click/type. Coordinates refer to the 1280 by 800 screenshot. Scroll over the intended pane and check `scroll_result.movement_observed`. Delivered input alone does not establish movement.
5. After navigation or a changing page, use `action: observe`, optionally with `wait_ms` up to 3000. A stale-observation rejection requires a fresh observation; do not repeat uncertain input blindly.
6. Read the retained HTML with `action: read_html`, `html_sha256`, `offset` and `max_bytes`. Chunks preserve UTF-8 boundaries and return `next_offset`. The artifact is bounded, explicitly truncated, and replaced by the next HTML capture.
7. Close the session when finished. Only one session is admitted; it expires after 60 seconds idle or five minutes total. Another authenticated principal cannot access it.

For prices, keep the model, capacity, colour, currency, payment/trade-in conditions and observation timestamp together. Rendered offer candidates use the existing native parser. A structured offer is not automatically the selected variant or a final checkout price. Missing stock, condition and delivery remain unknown. The HTML includes inline code; external script bundles are not automatically downloaded into the response.

## Limits and isolation

The controller launches a fresh temporary profile, retains Chromium's sandbox and owns its process tree through the existing native process supervisor. It does not attach to an existing logged-in browser. Chromium/CDP and its egress proxy listen only on loopback. DNS answers and connected peers must pass the public-address policy. Local networks, cloud metadata, URL credentials and nonstandard web ports are rejected. Fixed observation scripts run in an isolated world; MCP arguments cannot supply JavaScript or CDP methods.

There is no CAPTCHA solver, TLS-warning bypass, credential/profile import, download flow or proxy-identity rotation. Normal website access restrictions can still prevent retrieval, particularly from cloud IP addresses. The controller defaults to Chrome's unified headless mode. An operator can set `DEVBOX_BROWSER_HEADED=1` on Linux with an authorized `DISPLAY` and `XAUTHORITY` to use a visible X11 window instead. This is not an MCP argument.

For an Oracle deployment using the existing desktop supervisor, `DEVBOX_DESKTOP_BROWSER_AUTOSTART=0` keeps Xvfb, session D-Bus and Openbox available while the C++ controller owns the on-demand headed browser. This avoids a second permanently resident Chromium. The default remains unchanged; deployments must opt in after qualification. The prior persistent browser profile is preserved. The native desktop tools can observe the new window while its browser session is active.

The native proxy admits at most 48 connections and 64 MiB of traffic per session. Protocol messages, JSON complexity, HTML, text, controls, artifacts and operation durations are bounded. Linux startup checks host and container memory headroom; idle checks close the owned session under severe memory pressure. These checks are not a hard aggregate RSS guarantee. A small VM still needs enough memory for Chromium and its other services.

## Reproducible validation

`devbox-browser-tests` tests protocol framing, nested/out-of-order replies, command-error recovery, cancellation, deadlines, oversized replies and private-address denial. Add `--chromium` for real JavaScript price rendering, JPEG output, HTML hash integrity, page/nested-pane scrolling, click, principal ownership, stale-input rejection and explicit cleanup. Windows and Linux CI run the real browser fixture.

The opt-in comparison uses actual MCP calls against an isolated server root:

```text
DEVBOX_BROWSER_LIVE=1 node cpp-mcp/scripts/browser-live.mjs <binary> cpp-mcp/tests/fixtures/browser-public-corpus.json <new-evidence-directory>
```

It retains the exact binary hash, fixed denominator, per-URL HTTP and rendered outcomes, timing, output bytes, screenshots and bounded HTML. It never starts a production service or uses a user's existing browser profile. Review content and exact prices independently; an API call succeeding is not a successful source retrieval.

Initial local development comparison on 14 October 2026: seven of ten pages returned substantive content immediately; Currys and Argos returned access-denied pages, and Google required consent. Browser open/observation took 1.8–3.2 seconds in that sample. This is a small local-network development result, not an Oracle result, a 90% all-web claim or verified price accuracy across all retailers. Oracle qualification and ChatGPT connector validation are separate deployment gates.
