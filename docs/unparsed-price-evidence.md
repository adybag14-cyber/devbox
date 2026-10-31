# Visible amounts without parsed offers

A fresh Oracle Fallback check on 31 October 2026 retrieved the official Pixel 10
page successfully and retained its current starting price, previous price,
monthly payment and saving in visible text. Because no structured offer was
parsed, the offers view nevertheless labelled price evidence as `none`.

The C++23 offers view now distinguishes `unparsed_currency_text` from `none`
when accepted extracted text contains a common currency symbol/code followed
by an amount. It leaves the structured offer array and offer count unchanged.
The accompanying note requires inspection of the source and exact variant,
because the amount could be a starting price, payment, saving or unrelated
figure. It does not choose an amount or infer stock, tax, condition or delivery.

Parsed offers retain `source_reported`; their existing aggregate/exact-offer
assessments remain authoritative. Blocked responses remain excluded. Hidden
and executable page regions are excluded by the existing extraction step.
Currency detection is deliberately a text-presence signal, not a universal
currency parser or an exact-product-price claim. No additional request, browser
process or dependency is needed.
