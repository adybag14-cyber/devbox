# Native search and rendered-page reading

`devbox_web_search` discovers links without starting a long research job. It keeps titles, snippets, query/rank provenance, domain filters and provider failure details. Search metadata is not consulted source evidence. The existing standard 100-source and fast 50-source research workflows remain available.

`devbox_browser` is an optional C++23 controller for an installed Chromium browser. It adds no hosted browser, model, API-key or paid-proxy dependency. The core server can still run without Chromium; `action: status` reports availability. Set the operator-controlled `DEVBOX_BROWSER_EXECUTABLE` to an absolute executable path when automatic discovery is unsuitable.

On Windows, automatic discovery checks standard Chrome/Edge installation locations before scanning `PATH`. This avoids thousands of unsuccessful filesystem probes on large developer/CI paths and avoids preferring a PATH shim over an installed browser. Explicit executable configuration still takes precedence; portable installations retain the PATH fallback. Native qualification reports bounded startup phase timings and the selected executable/version without exposing a diagnostic MCP option.

## Agent workflow

A slow cold launch or page load can return `page_status: loading` and
`next_action: observe`. `pending_phase` distinguishes navigation not yet submitted
(`navigation`), an already-submitted document still loading (`document`), and a
settled page awaiting its snapshot (`observation`). The response includes the
owned `session_id` and `navigation_started`, but does not claim a page observation
or screenshot. Continue with `observe` on that same session. After submission,
this resumes reading/capture without issuing `Page.navigate` again. An explicit
negative navigation acknowledgement remains retryable by the caller.

The initial screenshot and HTML choices carry forward unless the observation
call explicitly overrides them. A soft navigation budget reserves five seconds
for capture or an honest continuation within the existing 40-second action
deadline. Input and old HTML are unavailable while navigation is pending; a real
fresh observation is required. Protocol failure still invalidates its connection
and cannot be treated as a successful continuation.

A disconnected channel retires its session as `protocol_closed`. A thrown
navigation error with an uncertain outcome retires it as `navigation_uncertain`;
it cannot yield an observation of the preceding page or automatically replay the
request. The owner's next status call exposes that terminal reason.

1. Use native search for discovery and `devbox_web_fetch` for inexpensive source retrieval. Use `max_age_seconds: 0` and `view: offers` for a fresh product check.
2. If useful content requires JavaScript or page interaction, call `devbox_browser` with `action: open`, the public URL, and optionally `include_html: true`.
3. Inspect `page_status`, URL, title, visible text, viewport text, controls, scroll regions and image. An access-denied page or consent screen is an intermediate outcome, not a successful source read.
4. Use the returned `session_id` and latest `observation_id` for scroll/click/type. Coordinates refer to the 1280 by 800 screenshot. Scroll over the intended pane and check `scroll_result.movement_observed`. Delivered input alone does not establish movement.
5. After navigation or a changing page, use `action: observe`, optionally with `wait_ms` up to 3000. A stale-observation rejection requires a fresh observation; do not repeat uncertain input blindly.
6. Read the retained HTML with `action: read_html`, `html_sha256`, `offset` and `max_bytes`. Chunks preserve UTF-8 boundaries and return `next_offset`. The artifact is bounded, explicitly truncated, and replaced by the next HTML capture.
7. Close the session when finished. Only one session is admitted; it expires after three minutes idle or ten minutes total. This leaves bounded time for an agent to interpret images and plan its next action. The existing memory-pressure cleanup can end it sooner. Another authenticated principal cannot access it.

Observations and HTML reads return `session_budget` with configured and remaining idle/hard milliseconds. Successful observations start a fresh idle allowance after capture; valid HTML reads also renew idle time. Failed requests and `status` do not keep a session alive, and the hard lifetime never renews. After cleanup, `last_session_end` exposes the most recent session ID, reason and time only to its authenticated owner; it retains no URL, image, text, HTML or cookies. Reasons distinguish `idle_timeout`, `hard_lifetime`, `resource_pressure`, `browser_exited`, `managed_browser_changed`, `open_failed`, `open_cancelled` and explicit `closed`. A matching owner request receives `BROWSER_SESSION_ENDED` with that reason; other principals still receive the generic not-found error. Open a new session and obtain a fresh observation before further input. The controller checks expiry under its lock before dispatch, so a request between janitor ticks cannot revive an expired session.

Owned-profile deletion uses a two-second retry window for transient filesystem errors after Chromium stops, including Windows handles that outlive process shutdown briefly. The exact session-named, non-symlink profile guard is retained; there is no scan or deletion of other profiles. Native qualification diagnostics record cleanup attempts when an explicit diagnostic callback is installed. The Windows fixture holds an owned profile file open across the former 250 ms retry window and verifies its eventual removal after release.

For prices, keep the model, capacity, colour, currency, payment/trade-in conditions and observation timestamp together. Rendered offer candidates use the existing native parser. A structured offer is not automatically the selected variant or a final checkout price. Missing stock, condition and delivery remain unknown. The HTML includes inline code; external script bundles are not automatically downloaded into the response.

HTTP 404/410 and recognized retailer soft-404 screens report `not_found`; their footer text must not be counted as product evidence. Challenge screens remain `challenge_required` even when their HTTP response is 403, while hard denials report `access_denied`. Observing a page again does not authorize solving a CAPTCHA or treating a challenge as content.

## Limits and isolation

Use `navigate(session_id,url)` for related pages in the same owned session. Some retailer product links depend on context established by the site's homepage: open the homepage, observe until it is stable and `content_available`, then navigate to the product. This preserves that session's normal first-party state. If a cookie panel obscures the product, choose the explicit required-only option from a fresh observation and capture the resulting page again. A challenge or denial remains an unavailable source; this flow does not solve challenges or import another browser's cookies. In a live Windows check, an Argos product denied on a fresh direct open was readable after this normal same-site navigation.

On Windows, owned Chromium sessions use the explicit `--enable-automation` launch option. This prevents Chrome from automatically relaunching through the interactive shell when the frontend is elevated, which would exit the tracked process before its DevTools endpoint is ready. The browser retains the frontend's token and remains inside Devbox's owned Job Object; Chromium's renderer sandbox, fresh profile and public-only network gate remain enabled. See [Chromium's elevation handoff implementation](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/app/chrome_main_delegate.cc). Qualification checks that the originally launched process survives rendering and terminates during cleanup, and must also run from the actual serving Windows session.

By default the controller launches a fresh temporary profile, retains Chromium's sandbox and owns its process tree through the existing native process supervisor. It does not attach to an existing logged-in browser. Chromium/CDP and its egress proxy listen only on loopback. DNS answers and connected peers must pass the public-address policy. Local networks, cloud metadata, URL credentials and nonstandard web ports are rejected. Fixed observation scripts run in an isolated world; MCP arguments cannot supply JavaScript or CDP methods.

There is no CAPTCHA solver, TLS-warning bypass, credential/profile import, download flow or proxy-identity rotation. Normal website access restrictions can still prevent retrieval, particularly from cloud IP addresses. The controller defaults to Chrome's unified headless mode. An operator can set `DEVBOX_BROWSER_HEADED=1` on Windows to open an owned Chromium window, or on Linux with an authorized `DISPLAY` and `XAUTHORITY` to use a visible X11 window. This is not an MCP argument. Windows headed sessions retain a fresh temporary profile, the sandbox, the public-only proxy, ownership checks and normal expiry; they do not attach to the operator's browser or import its cookies. The window is visible on the server's desktop, so operators should enable it only on an appropriate desktop session.

`DEVBOX_BROWSER_COMPATIBILITY=1` is a separate operator opt-in. The C++ launcher adds Chromium's `--disable-blink-features=AutomationControlled` switch, which changes its automation indicator without injecting page scripts or modifying credentials, TLS, sandboxing or network policy. This flag is also used in [Browser Use's launch profile](https://github.com/browser-use/browser-use/blob/main/browser_use/browser/profile.py). Status and observations disclose `compatibility_requested`; a managed Linux process must carry the exact switch before C++ accepts it. The Oracle supervisor applies it only to the private shared browser. A live local comparison reached an Argos product with its rendered GBP price where the same fresh headed session without this option received HTTP 403. That is limited compatibility evidence, not universal access or authorization to complete challenges.

For an Oracle deployment using the existing desktop supervisor, `DEVBOX_DESKTOP_BROWSER_AUTOSTART=0` keeps Xvfb, session D-Bus and Openbox available while the C++ controller owns the on-demand headed browser. This avoids a second permanently resident Chromium. The default remains unchanged; deployments must opt in after qualification. The prior persistent browser profile is preserved. The native desktop tools can observe the new window while its browser session is active.

Alternatively, `DEVBOX_BROWSER_SHARED=1` lets that supervisor keep one managed, sandboxed Chromium warm on Linux. This avoids repeated executable loading on a small VM. The browser uses a private temporary profile at `/tmp/devbox-desktop/browser`, never the operator's saved profile. C++ verifies the supervisor receipt, exact PID/start identity, command-line profile and private CDP endpoint before attaching. Each session creates a new incognito browser context with its own bounded public-only proxy. Cookies, local storage and pages are disposed on close, expiry or controller disconnect; closing a session does not terminate the managed browser. Process reuse is not browser-state reuse.

Shared mode overrides desktop browser autostart and waits up to 180 seconds for the private endpoint before marking the desktop ready. `devbox_browser status` reports warming/unavailable until then. The default blank browser context has no web egress; open a session with `devbox_browser` before using native X11 tools on its window. Native tools see the same headed page, and the session's normal expiry still applies. Warm reuse reduces startup work but does not remove the VM's memory limits or make blocked sites accessible.

The native proxy admits at most 48 connections and 64 MiB of traffic per session. Protocol messages, JSON complexity, HTML, text, controls, artifacts and operation durations are bounded. Linux startup checks host and container memory headroom; idle checks close the owned session under severe memory pressure. These checks are not a hard aggregate RSS guarantee. A small VM still needs enough memory for Chromium and its other services.

Container headroom accounts for reclaimable inactive file cache (`total_inactive_file` on cgroup v1 and `inactive_file` on v2), following the [Docker working-set convention](https://docs.docker.com/reference/cli/docker/container/stats/). Host `MemAvailable` still caps the result. Swap is not treated as free RAM; missing or inconsistent cache counters receive no credit. The existing 192 MiB opening threshold and 64 MiB idle-pressure threshold remain unchanged.

Managed-browser status also performs a read-only `Browser.getVersion` probe with a two-second total deadline and rechecks the process identity afterward. A surviving PID and stale endpoint file are insufficient readiness evidence. An unresponsive process reports `BROWSER_SHARED_UNRESPONSIVE`; the probe preserves existing contexts and never restarts another caller's browser. Opening a session bounds connection establishment and its initial version exchange to three seconds within the normal action deadline.

Owned browser processes disable background timer/process throttling and occluded-window backgrounding. On Windows they also disable native occlusion calculation for the controlled window. Headless captures activate the controlled page before requesting a screenshot, so a hidden target can produce its next frame. These scheduling controls follow [Chrome's automation guidance](https://github.com/GoogleChrome/chrome-launcher/blob/main/docs/chrome-flags-for-tools.md#task-throttling); the existing idle/hard session limits still bound unattended work. Native qualification can retain bounded Chromium launch diagnostics without exposing a diagnostic option to MCP callers.

`open` accepts `render_mode: "auto" | "full" | "text"`. Text mode rejects network Image and Media requests through native C++ CDP interception before download/decode. JavaScript, styles, fonts, readable text, controls, HTML and screenshots remain available; screenshots explicitly represent a reduced page. Inline graphics may remain. Each observation reports `render_mode`, `omitted_network_resources` and the reduced-rendering note. Full mode restores normal image loading for a new session, including when the managed process is reused.

Operators can set `DEVBOX_BROWSER_TEXT_ONLY=1` for the `auto` default on a constrained host; `status.default_render_mode` exposes the choice. The default without this setting is full rendering. This is a resource policy, not an access-control bypass. Prefer native `devbox_web_fetch` for structured prices, then text rendering when scripts or interactions are necessary, and full rendering for image-dependent work. The native interception queue and per-command drain are bounded and share the existing operation deadline.

Linux process launch also handles kernels or container policies that reject `close_range`. Its fallback enumerates the child's own open descriptors with allocation-free kernel calls, then retains the original close loop if enumeration is unavailable. This avoids scanning a million unused descriptor numbers and closes descriptors still open above a lowered soft limit. A separate CI target forces this fallback and exercises both foreground and detached descriptor isolation.

## Reproducible validation

Page settlement follows committed client redirects rather than waiting for the first navigation's loader forever. It excludes the preceding document and rechecks the final loader, complete URL and readable state after the settling interval. Navigation has a soft allowance of up to 30 seconds while reserving capture time inside the existing 40-second total action deadline. A still-loading document can continue in a later observation without being reloaded. Fast pages return after their first stable interval; cancellation remains active, and same-document fragment navigation does not require a new loader. This corrects controller budget handling, not a site's access policy. Real Chromium fixtures count page requests to verify that document and snapshot continuations do not replay navigation, and check principal ownership, stale-input rejection and capture-option preservation.

`devbox-browser-tests` tests protocol framing, nested/out-of-order replies, command-error recovery, cancellation, deadlines, oversized replies and private-address denial. Add `--chromium` for real JavaScript price rendering, JPEG output, HTML hash integrity, page/nested-pane scrolling, click, principal ownership, stale-input rejection and explicit cleanup. The managed-process fixture also verifies context proxy use, cookie/local-storage isolation from the default profile and preceding session, PID-identity rejection, context disposal, and survival of the managed browser. Windows and Linux CI run the real browser fixture in headless and headed modes; Linux uses Xvfb for the headed run.

The opt-in comparison uses actual MCP calls against an isolated server root:

```text
DEVBOX_BROWSER_LIVE=1 node cpp-mcp/scripts/browser-live.mjs <binary> cpp-mcp/tests/fixtures/browser-public-corpus.json <new-evidence-directory>
```

It retains the exact binary hash, fixed denominator, per-URL HTTP and rendered outcomes, timing, output bytes, screenshots and bounded HTML. It never starts a production service or uses a user's existing browser profile. Review content and exact prices independently; an API call succeeding is not a successful source retrieval.

Set `DEVBOX_BROWSER_LIVE_HEADED=1` to compare the owned headed mode. The separate `browser-retail-corpus.json` contains current retailer product URLs for that comparison. Keep the original ten-page corpus and its failures when reporting historical results: the Argos URL in that corpus returned the retailer's page-not-found screen in ordinary Chrome on 15 October 2026. A paired local diagnostic that day loaded the Currys product in fresh headed Chrome while unified headless returned HTTP 403 with the same proxy/sandbox flags. This is evidence for offering a normal headed mode, not a promise of universal access or a reason to count missing product pages as successful price retrievals.

Use `DEVBOX_BROWSER_LIVE_COMPATIBILITY=1` for the separate compatibility opt-in in this private comparison. Real browser fixtures accept `--compatibility` after either `--chromium` or `--chromium-headed`; CI runs all four independent combinations. They inspect the page-visible automation indicator in addition to the existing isolation and input checks. The comparison retains the last successful screenshot, including its observation identity/time, when a later observation fails. Such a failure remains a failed workflow and prevents input on the stale snapshot; retained diagnostic evidence is not presented as a later successful observation.

Initial local development comparison on 14 October 2026: seven of ten pages returned substantive content immediately; Currys and Argos returned access-denied pages, and Google required consent. Browser open/observation took 1.8–3.2 seconds in that sample. This is a small local-network development result, not an Oracle result, a 90% all-web claim or verified price accuracy across all retailers. Oracle qualification and ChatGPT connector validation are separate deployment gates.
