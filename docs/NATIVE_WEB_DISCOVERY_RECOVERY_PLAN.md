# Native research discovery recovery

## Observed problem

The live UK-phone-price session issued five native research jobs. Across 48 search queries, two returned results and 46 returned HTTP 202. The first job collected 44 documents from its small initial candidate set; the four follow-up jobs collected none. All five were labelled `candidate_exhaustion` even though provider failure prevented the requested searches.

A bounded direct probe confirmed the HTTP 202 body is DuckDuckGo's human-verification page. The code checks non-200 status before inspecting the body, its HTML detector uses the wrong anomaly marker, and its error `continue` skips the normal pacing delay. Provider state disappears when each research runner exits.

## Implemented C++23 changes

1. Classify challenge pages before generic HTTP failures; handle HTTP 202, 403, 429, transient errors and malformed search results explicitly. Never solve challenges, rotate identities, proxy around restrictions, or retry a blocked engine through another endpoint.
2. Share small, bounded provider pacing/cooldown state across jobs in the existing project cache. Stop a failing provider promptly and report why later jobs skip it. Keep cancellation and deadlines effective while waiting.
3. Add an independent public Bing RSS fallback for personal, non-commercial research, with its use notice visible in provider reports and documentation. Parse RSS natively with a bounded XML parser. Use only discovered source URLs; snippets and feed dates do not count as source evidence or publication dates. All source fetches retain the current address, robots, access, byte and relevance checks.
4. Record completed, failed and unattempted query counts and a discovery status separately from document count. Distinguish genuine candidate exhaustion from blocked, unavailable or incomplete discovery. Explicit seed URLs remain usable when all providers are unavailable. A target document count does not prove every requested entity was researched.
5. Preserve the existing Standard 100 and Fast 50 targets, tool names, durable-job identities and source-evidence interface. Update agent instructions and client registration as needed.

## Verification and release

- Reproduce the challenge-classification failure before the fix.
- Native fixtures cover HTTP 202 challenge detection, successful fallback, cross-job cooldown and pacing, no-result versus malformed markup, malformed/unsafe RSS, retained seed retrieval, partial query coverage, cancellation and output bounds.
- Replay the exact failed Apple pricing plan through a private MCP candidate; record actual usable sources, hosts, provider attempts, elapsed time and shortfall. Do not manufacture 50 sources or equate a parsed offer with verified availability.
- Run the existing native, MCP, compatibility and relevant hosted gates on the final commit. Review the final diff, merge the exact checked head, verify the merge tree and packaged artifact provenance, and activate only through the existing guarded lifecycle after active user jobs have drained.
- Verify authenticated capabilities, browser registration, health and recovery configuration after activation. Preserve other agents' chats, jobs, workspaces, Guardian and cloudflared.

## Provider limits

Public search availability is not guaranteed. Bing's RSS feed is an interface for personal, non-commercial aggregation, not an unrestricted paid-search API replacement; preserve that limitation and its attribution. If all providers are blocked or unavailable, the result must say so and guide the agent to supply independently discovered primary URLs. No credentials or paid service are introduced.

## Validation checkpoint

The HTTP 202 challenge regression failed on the previous implementation. The repaired native research fixtures pass, including persistent cooldown, fallback, per-query coverage, seed retrieval under provider outage, malformed RSS, longer Retry-After, pacing and cancellation. All 19 local native checks passed, as did the actual MCP engine SDK, frozen tool schema and durable agent smoke.

The exact failed Apple Fast request was replayed through a private candidate. It returned 32 qualifying documents from three hosts in 57.185 seconds, with all seven queries answered through the fallback. DuckDuckGo received one request, was classified as challenged, and was skipped for the remaining queries. The result correctly reports a shortfall of 18 against the 50-document target. These are source documents, not 32 verified purchasable offers; retailer access and availability still need source-level review. Final exact-commit platform certification and guarded deployment follow this checkpoint.

Review added structural recognition of the real empty-result element, with regressions rejecting CSS/script/comment/hidden lookalikes. A meta-only Windows-1252 price fixture exposed an invalid existing RE2 repetition bound; its encoding detection is corrected. A subsequent live replay reached the aggregate transfer budget and exposed a partial-chunk stop being mislabelled as candidate exhaustion. The native transport now preserves an explicit exhausted state, starts no further fetches, reports `byte_budget`, and resets the state for a new operation. Each issue was reproduced before its correction and regression-checked afterward.
