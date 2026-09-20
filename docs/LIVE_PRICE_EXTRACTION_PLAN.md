# Live phone-price extraction

The 20 September 2026 ChatGPT price comparison successfully fetched retailer pages, but the native output lost the relationship between each offer and the product options. AD UK Tech Store's Honor Win page exposed GBP 559 and GBP 536 as otherwise identical offers; its option data shows that GBP 536 requires bank transfer. Raw HTML text also contained a hidden sold-out badge while the rendered selected variant was available. Apple's product-family aggregate price is not an exact, in-stock variant offer.

## Implementation

- Keep research discovery and its Standard 100 / Fast 50 targets. A source count is not a count of verified phone offers.
- Add bounded native C++23 extraction of product-scoped Schema.org offers, including ProductGroup variants and in-document references. Preserve product identity, price currency, availability, seller, condition, price validity and conditional/recurring price qualifiers. Aggregate ranges stay explicitly separate.
- Join inert product variant JSON to an offer only by its explicit variant ID and the same product URL. Preserve option names and values, especially payment-dependent discounts. Never execute scripts or infer prices from unrelated analytics or snippets.
- Extend the existing fetch action with a compact `view: offers` and bounded offer pagination; keep provenance, current validation time, missing fields, conflicts and explicit failures. Invalidate the older normalized cache representation.
- Use existing public-network, robots, challenge, redirect and resource bounds. No new credentials, paid provider, model, or browser process is required.

## Acceptance

Regression fixtures must cover exact variants, bank-transfer discounts, wrong-currency prices, aggregate/from prices, sold-out and preorder offers, expired validity, conflicting variant data, ambiguous references, pagination and fresh revalidation. Verify live real phone pages on at least two retailers. Finally ask a separate logged-in ChatGPT 6 Pro conversation to use Devbox to retrieve current prices with exact variant, price conditions, stock as reported, retrieval time and source links. Compare its results against the visible retailer UI without purchasing anything.

Deploy only a clean source-bound build after local and hosted checks. Preserve the active agent's work, Guardian, tunnel and existing authentication. Record unsupported sites and coverage limitations honestly.
