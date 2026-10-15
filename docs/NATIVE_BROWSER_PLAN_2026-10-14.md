# Native web reading and browser plan

Status: implementation in progress in an isolated checkout. Windows production remains unchanged. Oracle Devbox Fallback is the first deployment/agent-validation target.

## Observed gap and comparison

Devbox currently has native HTTP search/fetch/extraction and a separate X11/Windows computer-use interface. Its HTTP fetcher does not execute page JavaScript, and its headed Chromium profile does not expose a coordinated DOM/text/screenshot interface to the agent. Source inspection also found that discovery discarded result titles and snippets after extracting URLs.

TinyFish's public integration calls its hosted MCP/API. Its free Search/Fetch offering is subject to limits and authentication; Browser and Agent are metered. The published93% usable-context fetch result,89.9% Online-Mind2Web result, and91.1% WebVoyager result refer to different evaluations. They are vendor-reported measurements, not an all-web access guarantee. Relevant ideas are browser rendering when needed, bounded extraction, independent batch failures, and returning compact results while keeping larger artifacts retrievable. No TinyFish runtime, paid proxy or model service is required by this implementation.

Sources inspected14October2026:

- https://www.tinyfish.ai/pricing
- https://www.tinyfish.ai/benchmarks
- https://docs.tinyfish.ai/fetch-api
- https://docs.tinyfish.ai/browser-api
- https://github.com/tinyfish-io/tinyfish-web-agent-integrations/blob/main/claude/README.md
- https://www.browserbase.com/pricing
- https://developer.chrome.com/docs/automation-and-testing/headless
- https://chromedevtools.github.io/devtools-protocol/

## Native implementation

1. **Fast discovery:** add `devbox_web_search` for one to four focused queries. Preserve ranked URLs, bounded titles/snippets, per-query provenance, domain filtering and provider failure/cooldown diagnostics. Do not count snippets as consulted documents. Retain the existing standard100/fast50 research workflows for substantive evidence collection.

2. **Owned browser session:** add an optional C++23 browser controller using Chromium's public DevTools protocol. Use an isolated temporary browser profile, record process identity, and retain Chromium's sandbox. A bounded native process supervisor owns the complete browser process tree and cancels only that tree. Oracle defaults to one active session, one controlled page, short inactivity expiry and a hard lifetime. Use the existing Chromium installation and existing C++ networking/JSON libraries.

3. **Coordinated observation:** return URL/title, visible readable text, compact interactive-element information, viewport/scroll position, a screenshot, and an observation identity from the same navigation state. Report instability or truncation. A page's HTML is an artifact with byte length/hash and bounded retrieval; large script/style bundles should not be injected into every agent response. Provide rendered HTML/source retrieval when the agent requests it.

4. **Bounded actions:** support navigation, observation, scrolling and explicit interactions using current session/observation identities. Detect navigation changes and stale input targets. Page contents remain untrusted data. No arbitrary model-provided JavaScript is evaluated. There is no autonomous CAPTCHA solver, credential harvesting, TLS-warning bypass or proxy-identity rotation.

5. **Network boundary:** browser execution must not weaken the existing public-web restrictions. Validate public HTTP(S) destinations and redirects, and prevent browser subresources from reaching loopback, private networks or cloud metadata. A native egress proxy can enforce destination/socket checks for HTTP and HTTPS tunnels without decrypting TLS. Disable proxy bypass/direct UDP paths in the owned browser. No public CDP/VNC listener is exposed. Keep source access restrictions and explicit challenge/block outcomes.

6. **Live offers:** feed captured rendered HTML through the existing native extraction/offer parser, preserving exact model, capacity, colour, condition, currency and payment qualifiers. Keep browser observation time separate from source publication and HTTP revalidation times. Missing stock/delivery fields remain unknown. Price extraction and visual/DOM availability are separate success measures.

## Qualification

- Preserve frozen legacy schemas and permissions; regenerate derived registry files using the repository generator.
- Native fixtures cover search metadata, domain bounds, byte budgets, provider failures and cancellation without retrieving source pages accidentally.
- Real Chromium fixtures exercise JavaScript-populated text/prices, long pages and scrolling, screenshots, HTML/artifact integrity, stale observations, navigation races, byte/time limits, private-address/redirect/subresource denial, cancellation and browser cleanup.
- Use a fixed public-site set including exact phone variants, JavaScript pages, documentation, tables and long pages. Measure access, usable content, correct variant/price, interaction success, latency, output bytes and agent calls separately. Excluded or blocked sites remain in the denominator with their reasons. Aim for at least90% on the declared set; do not generalize it to the entire web or silently replace failed URLs.
- Compare HTTP-only and rendered paths on the same candidate and Oracle egress to separate implementation effects from source/IP variability. Search and index snippets do not satisfy the50/100-source evidence target.
- Validate the final action through the logged-in ChatGPT website using Devbox Fallback explicitly. Retain the exact build identity and independent artifact/price evidence. Never fall back silently to the busy Windows Devbox Hermes connection.

## Deployment boundary

Oracle's current live build is9ce34fa7, contract9/53tools. The host has approximately1GB RAM and an existing headed Chromium/Xvfb runtime. SSH/probe latency and memory pressure must be diagnosed before staging another browser. Preserve unrelated services, the mail host, current jobs, credentials and Cloudflare routes. Qualify source/artifacts and a private canary first; drain the specific Fallback service before any serving-container replacement. Never run two frontends against shared live state.

Windows production rollout is outside this task's first-stage deployment. It remains on its existing validated build while Oracle is used for the new functionality and evidence.
