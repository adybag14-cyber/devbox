# Private coordinator read transport

## Scope

Follow-on performance experiment based on PR78 commit `643b99af3c58fa8cfae341b66b2ca213e48e3183`. The baseline already reuses compiled SQLite reads. This patch targets the HTTP connection setup around those queries rather than caching returned records.

`StateClient` opts in for `get`, `count`, `count_matching`, `list`, `events` and `diagnostics`. Mutation, import, snapshot and stop operations retain fresh transport. The native `StateClientOptions::reuse_read_connections` comparator is not exposed as an MCP argument or environment setting.

The optional transport scope admits only POST to the exact numeric-loopback form `http://127.0.0.1:PORT/mcp`, with direct proxy bypass. It cannot follow redirects. One idle easy handle is retained per caller thread, not an unbounded URL/tenant map. The scope binds process ID, process instance and writer generation; the URL also binds the port. An identity or URL change closes the previous handle. Existing state authentication, encryption, nonce/replay limits and response proof verification are unchanged.

The handle is exclusively borrowed, reset before request-owned buffers and headers die, and returned only after a successful HTTP 200 transfer. Transport failure, timeout, oversized response and non-200 replies discard it. Responses and credentials are never put in this cache. Every call still validates coordinator process identity and authenticates its own encrypted request and reply. A successful HTTP transfer is not itself a successful authenticated state operation.

Libcurl is configured for one cached connection, at most five seconds idle before reuse, and at most sixty seconds total connection age. These are reuse eligibility limits, not a separate timer that proactively closes idle sockets. Existing server idle timeouts still apply, and thread exit destroys the handle. Server connection limits and all scheduling/admission limits remain unchanged.

## Verification

The native `local-http-reuse` fixture checks fresh-versus-reused connection identity, body/header rebinding, scope and port changes, peer closure, cancellation, transfer timeout, response limits, redirect refusal and concurrent caller isolation. Existing coordinator tests retain their encrypted payload, proxy bypass, spoofed acknowledgement, restart, generation and idempotent batch checks.

`devbox-ipc-read-bench` compares both native options in one executable against a new private coordinator. Each trial checks exact data, revisions and owner identity. Maximum samples are bounded so the test respects the coordinator's nonce capacity. The smoke test asserts correctness, not a machine-dependent latency threshold. Security lanes select the new transport test and IPC benchmark in addition to existing suites.

Example, after building:

```text
ctest --test-dir BUILD -C Release -R "^(local-http-reuse|ipc-read-benchmark-smoke|state-coordinator)$" --output-on-failure
BUILD/cpp-mcp/Release/devbox-ipc-read-bench.exe NEW_PRIVATE_DIRECTORY 256
```

Matched full MCP testing uses the same `matched-benchmark.mjs` harness as PR78 and retains the health control route. Native coordinator timing and complete HTTP/MCP request timing must be reported separately. Existing SQLite-only microbenchmarks do not establish complete connector latency. No public ChatGPT/Cloudflare or remote model timing is implied.

Results belong to exact source and binary hashes. Initial development builds use the established Windows compiler/dependency configuration; they are not signed shipping artifacts. Qualification evidence and failed trials must be retained rather than relabelled.

## Primary libcurl references

- Easy handle reuse: https://curl.se/libcurl/c/libcurl-easy.html
- Reset retains live connections, but resets option pointers: https://curl.se/libcurl/c/curl_easy_reset.html
- Connection-count bound: https://curl.se/libcurl/c/CURLOPT_MAXCONNECTS.html
- Idle eligibility: https://curl.se/libcurl/c/CURLOPT_MAXAGE_CONN.html
- Redirect behavior: https://curl.se/libcurl/c/CURLOPT_FOLLOWLOCATION.html

No production rollout, public schema change, weaker durability, broader authority or machine-wide timer setting is part of this patch.
