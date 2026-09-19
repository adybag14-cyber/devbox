# Native public-web research

The C++23 service adds three research tools to the frozen compatibility and computer-use surface. Contract version 4 advertises 50 tools. Research uses native libcurl networking and Lexbor HTML parsing, with no Python, browser farm, embedding model or external LLM in the implementation.

## Research presets

| Mode | Source target | Running-time budget |
|---|---:|---:|
| `standard` | 100 usable unique documents | 300 seconds |
| `fast` | 50 usable unique documents | 120 seconds |

These are collection targets, not promises that every question has 100 relevant accessible sources. A narrow or blocked search returns its actual coverage, exclusions, distinct-domain count and shortfall. A technically completed job can have partial coverage. Do not pad an answer with unrelated pages, count search snippets as documents, or equate 100 documents with 100 independent publishers.

## Agent workflow

Call `devbox_web_research` with a stable task/operation identity, a topic and focused query variants. Supply known primary URLs when possible. Use `domains` for niche source restrictions and `exact_terms` for products, chips or other named entities. Quoted query phrases become the entity filter when explicit exact terms are absent. A listed term must occur in substantive source evidence; form/dropdown noise does not qualify.

```json
{
  "task_id": "phone_research",
  "operation_id": "research_20260919",
  "topic": "Snapdragon 8 Elite Gen 5 phone prices in the UK",
  "mode": "fast",
  "queries": ["Snapdragon 8 Elite Gen 5 phone prices"],
  "exact_terms": ["Snapdragon 8 Elite Gen 5"],
  "domains": ["e-catalog.co.uk"],
  "urls": ["https://e-catalog.co.uk/list/122/pr-56675/"],
  "max_age_seconds": 0
}
```

The call returns promptly with `job_id`. Repeating the same task/operation/input returns the original job; changed input under that identity conflicts. Existing `devbox_job_status` supports a passive wait. `devbox_job_cancel` requests cancellation asynchronously and may return `cancel_requested` while the runner or child is still active. Poll `devbox_job_status` until a terminal state confirms completion.

Read `devbox_web_evidence` and follow `next_offset` until all source briefs have been inspected before claiming that the requested number of sources was consulted. `source_id` retrieves a larger immutable excerpt, tables and metadata. If `minimum_required_chars` appears, increase the output budget instead of repeating a non-advancing cursor. Source records do not become proof of truth merely because they were retrieved successfully.

```json
{"job_id":"<returned ID>","limit":100,"max_chars":64000}
```

```json
{"job_id":"<returned ID>","source_id":"s1","query":"priceCurrency","max_chars":128000}
```

For known sources, `devbox_web_fetch` retrieves up to 16 URLs in one call, with excerpts, bounded tables/metadata and explicit failures. Duplicate input URLs are fetched once. `remaining_url_indexes` identifies omitted records by zero-based index in the original input. Increase `max_chars` or request a smaller batch for more detail; research jobs preserve immutable evidence for later drill-down.

## Keyless discovery and source access

`discovery` selects `web` (best-effort public DuckDuckGo HTML), `scholarly` (Crossref discovery followed by publisher retrieval), `encyclopedia` (MediaWiki discovery followed by page retrieval), or `none` (seed URLs only). No paid API or API key is required. Public interfaces can change, block requests or omit relevant sites; this does not guarantee the coverage of a proprietary index. The agent can supply additional seed URLs from its own search. Only one bounded link-expansion step is performed.

Source retrieval respects robots rules and reports unavailable policies, access blocks and challenges. It does not bypass authentication, paywalls or CAPTCHAs. Only public HTTP(S) and standard web ports are eligible. Resolved addresses and redirects are validated; inherited proxies, netrc and cookies are not used. TLS verification stays enabled.

HTML, plain text and structured JSON/XML are read within explicit bounds, without executing JavaScript. UTF-8 supports multilingual content; Windows-1252 and Latin-1 are also decoded. Unsupported encodings, PDF, JavaScript-only content and access blocks are explicit outcomes, not fabricated text. A separate authorized reader may be needed for those sources.

## Evidence quality and freshness

Records retain their URLs, retrieval/validation times and hashes. Exact content duplicates do not inflate coverage. Reported publication/modification dates remain separate from HTTP Last-Modified and retrieval time. A nested product's date is not promoted to the publication date of a catalogue.

Product offers retain their parent name, URL and JSON location. Price, currency, availability and validity fields remain source claims. Exact phrase matching is a relevance filter, not proof of a chip, price or release status. Reconcile conflicting sources and distinguish rumors, announcements, availability, variants, markets and currencies before synthesis.

Use `max_age_seconds: 0` for volatile facts. This revalidates cached documents where validators are available. Positive values, capped at one hour, permit labeled cache reuse. A fresh response can still contain old information; stale data is not silently substituted for a fresh verification.

## Resource bounds and lifecycle

- Up to four concurrent transfers and one per origin per client, with connection reuse.
- Up to 256 candidate documents, 16 queries and 16 target domains per job.
- At most 2 MiB decoded per response and 64 MiB decoded per research operation.
- A 64 MiB normalized-document cache. Evidence lives in the existing owned job directory and follows job retention.
- One collecting research job per project. Other research jobs wait while queued, before consuming execution slots, and remain cancellable.
- Existing commands, computer use and health routes retain their own admission paths.
- Text, tables, JSON-LD and output pages have explicit limits and truncation indicators.

Telemetry records `usage_type: web_research` and redacts topic/query/URL/domain/exact-term previews. The private job request and source ledger retain recovery and citation evidence. Fetch/evidence reads use `mcp:devbox:read`; submission uses `mcp:devbox:exec`. Existing broad `mcp:tools` authorization remains compatible. Source content is untrusted data, never permission or instructions.

## Verification and client refresh

Native fixtures exercise malformed HTML, entities, currency/Unicode, scoped metadata, exact entities, deduplication, full 50/100-document runs, partial coverage, connection reuse, concurrency, size/output bounds, private-address/redirect rejection, rate limits, cache revalidation and cancellation. The opt-in `cpp-mcp/scripts/web-research-live.mjs` tests the actual MCP against public sources and records true coverage. Its RFC case is a test of real documents from one publisher, not 100 independent publishers.

After deploying a certified candidate, refresh the ChatGPT connection and verify the three tool names and schema hash. Restarting the server does not update a client's cached tool registration.
