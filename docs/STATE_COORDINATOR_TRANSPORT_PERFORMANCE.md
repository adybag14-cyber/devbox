# State coordinator transport refinement

Date: 26 September 2026. Follow-on to PR #78; production is not changed by this branch.

## Measured bottleneck and selected approach

An instrumented copy of #78 measured a small native coordinated read at approximately 1,028 microseconds median on the local Windows workstation. Endpoint identity checking took about 10 microseconds, request crypto/JSON 25 microseconds, HTTP round-trip 939 microseconds, and response proof/decryption/JSON 44 microseconds. The HTTP interval includes the coordinator's work; it is not exclusively TCP setup. These are a profiling pilot, not release performance claims.

The prior generic HTTP helper destroys its curl handle after each exchange. This candidate gives only the private state protocol a dedicated, bounded connection pool. The generic network helper, provider adapter and public HTTP server are unchanged. Compare ephemeral connections and reuse in the same binary, then compare the whole MCP request path with a matched #78 executable before attributing any whole-request benefit.

## Invariants

- Exactly `http://127.0.0.1:<validated-port>/mcp`: no caller-supplied host, path, headers, proxy, credentials or redirect destination. Redirect following and ambient proxy/netrc use are disabled.
- At most four idle easy handles per StateClient; each handle caches at most one connection. A borrowed handle has exclusive ownership. Excess concurrent requests use ephemeral handles rather than waiting on pool capacity or increasing its idle bound.
- Every request retains fresh nonce/IV, encryption, HMAC, response proof, generation and process-instance checks. A warmed socket is never authentication. A transfer enters the idle pool only after nonce, generation, MAC and ciphertext have been verified. An authenticated application error may reuse a channel; an invalid proof cannot.
- A port/PID/instance/generation change retires idle connections. Retry invalidation advances a pool epoch, preventing an earlier in-flight request from repopulating the retired pool.
- Request options and borrowed buffers are detached before return, including exception paths. Body/header callbacks are bounded and do not allow C++ exceptions through libcurl's C ABI. Idle age eligibility is ten seconds and maximum connection lifetime is sixty seconds; server idle closure and libcurl stale-socket detection remain effective.
- No record, authorization decision, grant, snapshot or writer generation is cached. SQLite transactions, FULL durability, CAS checks, batch receipts, replay protection and the two-attempt recovery bound remain unchanged.
- `StateClientOptions::reuse_connections` is native qualification input only. There is no new environment variable, public tool, permission, or schema version.

## Qualification

`state-transport` verifies actual connection counts (12 transfers require 12 connections without reuse and one with reuse), exclusive borrowing, four-idle capacity, eight concurrent callers, peer/epoch invalidation, oversize bodies/headers, timeouts, pre/in-flight cancellation, redirects and recovery after failures. The coordinator suite warms a verified forwarding connection and then substitutes an impostor reply: every response must still prove its identity. Existing proxy refusal, frontend survival, cross-client receipt replay and coordinator restart tests remain.

`state-ipc-benchmark-smoke` makes no speed-threshold assertion. The same executable supports `--samples 1000` for a complete ABBAABBA comparison. Each reported duration includes the full native client get and its correctness oracle. It includes encryption, local HTTP, coordinator execution and SQLite; it excludes the outer MCP frontend/public network/model. Raw observations must be retained with executable hashes. Native tests and both ASan/static-analysis and ThreadSanitizer selections include the new tests.

Run the ordinary platform/security matrix and exact-head SDK/canary qualification. Report skipped/excluded local desktop suites separately. Pilot builds sharing a prior common library are not final release evidence. Do not merge or deploy based only on local timing.

## Rejected alternatives

Response/authorization caching, bypassing the state coordinator for local reads, removing cryptography or process-instance checks, disabling durability, replacing bounded queues with spinning, and machine-wide timer changes are not part of this patch. They would change safety or measurement conditions rather than remove this measured avoidable connection setup.

## Primary references

- libcurl reset semantics: https://curl.se/libcurl/c/curl_easy_reset.html (options reset; connections and other caches are distinct).
- Exclusive handle ownership: https://curl.se/libcurl/c/threadsafe.html.
- Connection cache bound: https://curl.se/libcurl/c/CURLOPT_MAXCONNECTS.html.
- Idle/lifetime eligibility: https://curl.se/libcurl/c/CURLOPT_MAXAGE_CONN.html and https://curl.se/libcurl/c/CURLOPT_MAXLIFETIME_CONN.html.
- Existing state proof and replay contract: `cpp-mcp/src/state_coordinator.cpp` and `cpp-mcp/tests/state_coordinator_tests.cpp`.
