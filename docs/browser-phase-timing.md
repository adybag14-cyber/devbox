# Browser phase timing

Native browser results retain the existing `timing.action_ms`, capture timings
and cumulative protocol counters. An `open` response also includes
`timing.startup`, which separates these consecutive phases:

| Field | Measured work |
|---|---|
| `resolve_and_proxy_ms` | Resolve the browser, check memory and prepare the private egress proxy |
| `endpoint_ready_ms` | Validate the managed process or launch the owned process and wait for its debugger endpoint |
| `cdp_connect_ms` | Read the endpoint and establish its loopback debugger connection |
| `session_setup_ms` | Read browser version, prepare isolation, attach the page and configure capture/network domains |
| `total_ms` | All startup phases together, before initial page navigation |

The phases use the native monotonic clock and integer milliseconds. Their sum
can differ from `total_ms` by up to three milliseconds due to rounding. The same
measurements are available when a slow open returns an owned session with
`next_action=observe`. They contain durations only, without executable paths or
new page data. Subsequent ordinary observations do not repeat startup metadata.

Initial navigation, explicit `navigate`, and their continuation responses include
`timing.navigation.submission_ms` and `timing.navigation.document_wait_ms`.
These accumulate active work for that navigation across continuation calls; time
spent waiting for the agent's next call is excluded. A continued navigation does
not issue another successful `Page.navigate`. The existing action and observation
timings still describe the current call. Ordinary settled-page observations do
not repeat navigation metadata.

These fields help distinguish slow browser startup, page navigation and first
render/capture work. They do not add browser commands, change readiness or
cancellation rules, increase deadlines, or establish a speed improvement.
Real Chromium tests check the phase partition and continuation reporting in
both owned and managed isolated contexts.
