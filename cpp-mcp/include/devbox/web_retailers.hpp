#pragma once
#include "common.hpp"
namespace devbox::web {
// Identity only: never use this to redirect a request or infer independent publishers.
std::string source_identity_url(std::string_view url);
Json retailer_offer_records(const Json& document);
void annotate_offer_consistency(Json& row);
Json match_product_targets(const Json& document, const Json& targets, bool require_offers = true);
} // namespace devbox::web
