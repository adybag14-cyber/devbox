# Native C++23 web research upgrade

This plan records the design grounded in a live pricing-research session on 19 September 2026. The implemented interface is documented in [NATIVE_WEB_RESEARCH.md](NATIVE_WEB_RESEARCH.md). Validation and deployment identity must be checked against the actual candidate and live capability manifest.

## User requirements

- Research normally targets at least 100 sources per answer, with a Fast preset targeting 50.
- A genuinely narrow question can have fewer usable sources. Return the actual count and explain the shortfall; do not pad the ledger with irrelevant pages or label search snippets as consulted sources.
- No API keys, paid search/scraping subscriptions, or external LLM services are required by the upgrade.
- Implement the new behavior in C++23 and validate it with meaningful native, transport, live-network and resource tests.
- Preserve the agents currently using Devbox and their independent jobs. Do not replace a live service while it is busy.

## Observed shortcomings

A live ChatGPT pricing-research session asked an agent to find prices for phones using ten listed SoCs. Private conversation identifiers and raw telemetry remain in local audit evidence rather than the repository.

Telemetry shows that the agent had to construct curl/Python/BeautifulSoup requests and repeatedly inspect HTML through generic execution tools. Confirmed failures include a nested PowerShell quoting error at 16:48:36 UTC and a Python cp1252 UnicodeEncodeError at 16:51:12 UTC. The same E-Catalog category page was downloaded repeatedly while the agent guessed selectors. A raw response consumed more than 24,000 output characters. Process exit success did not prove useful web evidence: some successful Python calls returned only 73–206 output characters, and one extraction returned nine.

Other live chats are using the same MCP simultaneously. The client ID in telemetry is shared, so timestamps/client IDs alone cannot attribute every call to the SoC conversation. Attribute research calls by their request previews and confirm the workflow through the visible chat. Do not include unrelated Rust-porting activity in research efficiency measurements.

The current MCP has native process/file/CUA primitives but no structured public-web retrieval or research ledger. The upgrade should remove repeated shell quoting, interpreter startup, parser authoring, redundant network downloads and raw-page output from the agent's critical path.

## Agent-facing design

Three additive native tools, preserving the existing 47 schemas:

1. `devbox_web_fetch`: fetch a bounded batch of explicit public URLs, normalize content, return provenance, useful text, tables and structured product/article metadata, and explicitly classify inaccessible/unsupported pages. Use this for inspecting a known niche source or drilling into evidence.
2. `devbox_web_research`: submit an idempotent durable research job with a topic, query variants, optional explicit URLs/domain restrictions, and `standard` or `fast` mode. Standard targets 100 usable documents; Fast targets 50. The first call returns quickly with a durable job ID instead of holding the MCP request open through all network operations.
3. `devbox_web_evidence`: read the job's authoritative status, coverage, source ledger and selected document evidence in bounded pages. Results include citation IDs, URLs, retrieval times, reported publication/modification times, content hashes, source type, deduplication and failure reasons.

Reuse the existing C++ durable-job ownership, receipts, heartbeat, cancellation, scheduler and log mechanisms. Add a native research job mode rather than spawning Python, a browser farm, or another service. The native job runner performs the research function inside the same verified C++ executable. Existing job-status/cancel operations remain useful.

Tool descriptions must guide the agent to consume the evidence ledger before asserting a source count. Retrieval/parse success is not proof that a factual claim is true or that the agent considered all evidence. Search hits, blocked pages, empty shells and identical syndicated content do not inflate usable-source coverage.

## Retrieval and discovery

- Implement bounded parallel HTTP with libcurl multi, connection reuse and a small per-origin limit; do not create a new interpreter/process for each URL.
- Support explicit URLs and arbitrary public target domains. A finite site catalogue is not the coverage claim or a hard allowlist.
- Use public keyless discovery where it works, with provider-specific status and backoff. Prefer documented specialist APIs for matching research: MediaWiki and Crossref are initial verified candidates. General search HTML is best effort and can become blocked or change format.
- Do not make a public SearXNG instance or an unauthenticated search feed a guaranteed dependency. Public instance formats can be disabled, and rate limits apply. Do not rotate identities/proxies or bypass CAPTCHAs, logins, paywalls or access controls.
- Accept seed URLs from the agent's own search when keyless discovery is incomplete. Apply the same content, provenance and duplicate checks to all sources.
- Parse HTML natively with a real bounded parser. Ignore executable scripts, preserve useful table structure, and extract bounded JSON-LD article/product/offer metadata as source claims. Do not execute page JavaScript.
- Clearly separate publication date, HTTP Last-Modified, retrieval time and cache age. A fresh HTTP response does not establish that an article or price is current.
- Price evidence must retain currency, availability/variant context and source URL. Rumored/unreleased products must not silently become available products just because a search hit names them.

## Resource and network boundaries

Initial budgets will be finalized against measurements: approximately four concurrent transfers, one or two per origin, bounded decompressed response sizes, a finite candidate/request count, separate Standard/Fast deadlines, and bounded local cache/evidence storage. Avoid a GPU model, embedding database or always-running browser pool.

Use conditional revalidation and explicit cache ages. Price/current-event queries need a fresh mode. Stale fallback must be labeled; it cannot count as a fresh verification. Keep enough evidence for reproducible citations without returning entire pages to the agent.

Only public HTTP(S) endpoints are eligible. Block credentials in URLs, non-web protocols, local/private/link-local addresses, cloud metadata endpoints and unsafe redirects. Enforce the resolved-address boundary at connection time, including redirects; disable inherited proxies/netrc/cookies for this public-web path. Keep TLS verification enabled. Browser-authenticated access remains a separate user-authorized surface.

Respect robots policies and rate-limit responses; record exclusions rather than hiding them. Partial provider or document failures must not abort an otherwise useful batch or masquerade as 100 consulted sources. Cancellation must stop network activity and preserve explicit partial/terminal state.

## Accuracy and coverage contract

The research backend returns evidence, not an invented synthesized answer. It must expose:

- requested target (100/50), candidate/discovered counts, attempted URLs, usable unique documents, distinct domains, duplicates and failures;
- whether the target was reached and a specific shortfall reason;
- provider results separately from document retrieval results;
- stable source IDs and hashes, snippets tied to actual extracted text, date provenance, and links back to source documents;
- pagination/completeness markers so truncated output is never mistaken for a complete review.

A niche query with 18 useful documents should report 18 and the missing coverage. A benchmark that reads 100 documents from one publisher must report one distinct domain, not claim 100 independent publishers. Count alone does not establish quality, independence or consensus.

## Validation gates

1. Native fixtures for HTML/entities/Unicode/tables/JSON-LD, canonical URL handling, query matching, deduplication, redirects, size limits, partial failures and challenge detection.
2. Real local HTTP fixtures for concurrent requests, per-origin bounds, cache/304 behavior, timeout, Retry-After, cancellation and private-address rejection. Test-only transport injection must not expose a production bypass.
3. Durable-job integration for idempotent submission, conflict rejection, restart/readback, cancellation and correct terminal status.
4. Native MCP schema/auth/capabilities tests; retain all frozen Rust compatibility checks and current CUA behavior.
5. Live keyless probes using the actual host network, including the SoC/E-Catalog case and a niche primary-source case. Exercise both the 50- and 100-source targets and record actual usable counts, failures, time, bytes and peak resource use.
6. Compare repeated retrieval against cached/conditional retrieval and native batching against the observed repeated-script workflow. Report measured results and limits, not an unmeasured claim of Perplexity equivalence.
7. Certify the exact final source head and merged tree before production activation. Wait for active agents/jobs to quiesce, preserve supervisor/tunnel/OAuth state, retain the prior immutable C++ executable, refresh the plugin schema, and verify live behavior.

## Primary implementation references

- libcurl multi and connection reuse: https://curl.se/libcurl/c/libcurl-multi.html
- Resolved-address connection guard: https://curl.se/libcurl/c/CURLOPT_OPENSOCKETFUNCTION.html
- MediaWiki search API: https://www.mediawiki.org/wiki/API:Search
- Crossref access and authentication: https://www.crossref.org/documentation/retrieve-metadata/rest-api/access-and-authentication/
- SearXNG search API and public-instance limitations: https://docs.searxng.org/dev/search_api.html

These references inform implementation choices; live availability and content quality still require measurement.
