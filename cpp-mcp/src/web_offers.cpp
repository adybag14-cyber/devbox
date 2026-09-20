#include "devbox/native.hpp"
#include "devbox/research.hpp"
#include <algorithm>
#include <charconv>
#include <map>
#include <re2/re2.h>
#include <set>

namespace devbox::web {
namespace {
constexpr std::size_t max_offers = 128, max_offer_bytes = 128 * 1024;
std::string text(const Json& value, std::string_view key, std::size_t bound = 500) {
    const auto found = value.find(std::string(key));
    if (found == value.end())
        return {};
    auto out = found->is_string() ? found->get<std::string>() : found->is_number() ? found->dump() : "";
    return evidence_excerpt(out, "", bound);
}
std::string schema_name(std::string name) {
    for (const auto prefix : {"https://schema.org/", "http://schema.org/"})
        if (name.starts_with(prefix))
            return name.substr(std::string_view(prefix).size());
    return name;
}
bool type_is(const Json& value, std::string_view kind) {
    if (!value.is_object() || !value.contains("@type"))
        return false;
    const auto& type = value["@type"];
    if (type.is_string())
        return schema_name(type.get<std::string>()) == kind;
    if (type.is_array())
        for (const auto& entry : type)
            if (entry.is_string() && schema_name(entry.get<std::string>()) == kind)
                return true;
    return false;
}
std::string named(const Json& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    return found != value.end() && found->is_object() ? text(*found, "name") : text(value, key);
}
std::string safe_url(std::string_view raw, std::string_view base) {
    if (raw.empty())
        return {};
    try {
        return normalize_url(raw, base);
    } catch (...) {
        return {};
    }
}
std::string price(const Json& value, const char* key) {
    auto amount = text(value, key, 80);
    std::string whole, fraction;
    static const RE2 decimal("([0-9]{1,12})(?:\\.([0-9]{1,2}))?");
    if (!RE2::FullMatch(amount, decimal, &whole, &fraction))
        return {};
    while (whole.size() > 1 && whole.front() == '0')
        whole.erase(0, 1);
    fraction.resize(2, '0');
    return whole + "." + fraction;
}
std::string variant_id(std::string_view value) {
    try {
        const auto url = Url::parse(value);
        std::string result;
        for (const auto& item : split(url.query, '&', false)) {
            if (!item.starts_with("variant="))
                continue;
            const auto id = item.substr(8);
            if (!RE2::FullMatch(id, "[0-9]{1,24}") || !result.empty())
                return {};
            result = id;
        }
        return result;
    } catch (...) {
        return {};
    }
}
bool same_product(std::string_view left, std::string_view right) {
    try {
        const auto a = Url::parse(left), b = Url::parse(right);
        return a.origin() == b.origin() && a.path == b.path;
    } catch (...) {
        return false;
    }
}
void field(Json& to, const Json& from, const char* source, const char* target = nullptr) {
    const auto value = text(from, source);
    if (!value.empty())
        to[target ? target : source] = value;
}
struct Extractor {
    std::string base;
    const Json& variants;
    std::map<std::string, const Json*> references;
    std::set<std::string> duplicate_ids, seen;
    Json records = Json::array();
    bool truncated = false;
    std::size_t visited = 0, bytes = 0;

    void index(const Json& value, unsigned depth = 0) {
        if (depth > 20 || ++visited > 4096) {
            truncated = true;
            return;
        }
        if (value.is_object()) {
            const auto id = text(value, "@id", 2048);
            if (!id.empty() && value.size() > 1 && !references.emplace(id, &value).second)
                duplicate_ids.insert(id);
            for (const auto& entry : value.items())
                if (entry.value().is_structured())
                    index(entry.value(), depth + 1);
        } else if (value.is_array())
            for (const auto& entry : value)
                index(entry, depth + 1);
    }
    const Json& resolve(const Json& value) const {
        const auto id = text(value, "@id", 2048);
        if (value.is_object() && value.size() == 1 && !id.empty() && references.contains(id) &&
            !duplicate_ids.contains(id))
            return *references.at(id);
        return value;
    }
    void enrich(Json& row) {
        const auto id = variant_id(json_string(row, "url"));
        if (id.empty())
            return;
        row["variant_id"] = id;
        const Json* match = nullptr;
        for (const auto& variant : variants) {
            if (text(variant, "id") != id ||
                !same_product(text(variant, "product_url", 2048), json_string(row, "url")))
                continue;
            if (match && *match != variant) {
                row["conflicts"].push_back("variant_metadata_ambiguous");
                return;
            }
            match = &variant;
        }
        if (!match)
            return;
        field(row, *match, "title", "variant_name");
        if (match->contains("options"))
            row["options"] = match->at("options");
        if (match->contains("option_names"))
            row["option_names"] = match->at("option_names");
        if (json_string(row, "variant_name").empty() && row.contains("options")) {
            std::string label;
            for (const auto& option : row["options"])
                if (option.is_string()) {
                    if (!label.empty())
                        label += " / ";
                    label += option.get<std::string>();
                }
            row["variant_name"] = evidence_excerpt(label, "", 500);
        }
        const auto sku = text(*match, "sku");
        if (!sku.empty()) {
            if (!json_string(row, "sku").empty() && row["sku"] != sku)
                row["conflicts"].push_back("variant_sku_disagrees");
            else
                row["sku"] = sku;
        }
        if (match->contains("available") && match->at("available").is_boolean()) {
            row["variant_available_reported"] = match->at("available");
            const auto availability = json_string(row, "availability");
            if ((!match->at("available").get<bool>() && availability == "InStock") ||
                (match->at("available").get<bool>() && availability == "OutOfStock"))
                row["conflicts"].push_back("variant_availability_disagrees");
        }
        if (json_bool(*match, "requires_selling_plan"))
            row["qualifiers"].push_back("requires_selling_plan");
        // Values stay attached to their labels. A lower bank-transfer price is not an unconditional deal.
        if (row.contains("option_names"))
            for (std::size_t i = 0; i < row["option_names"].size(); ++i) {
                const auto name = lower(row["option_names"][i].get<std::string>());
                if (name.find("pay") != name.npos || name.find("bank") != name.npos ||
                    name.find("trade") != name.npos || name.find("contract") != name.npos ||
                    name.find("subscription") != name.npos)
                    row["qualifiers"].push_back(
                        "payment_or_eligibility_option: " + row["option_names"][i].get<std::string>() +
                        " = " + row["options"][i].get<std::string>());
            }
        row["variant_evidence"] = text(*match, "evidence_path");
    }
    void offer(const Json& raw, const Json& product, const std::string& path, unsigned depth) {
        if (depth > 16 || ++visited > 2048 || records.size() >= max_offers) {
            truncated = true;
            return;
        }
        const auto& value = resolve(raw);
        if (value.is_array()) {
            for (std::size_t i = 0; i < value.size(); ++i)
                offer(value[i], product, path + "[" + std::to_string(i) + "]", depth + 1);
            return;
        }
        if (!value.is_object())
            return;
        const bool aggregate = type_is(value, "AggregateOffer");
        if (!type_is(value, "Offer") && !aggregate)
            return;
        const auto& identity = value.contains("itemOffered") ? resolve(value["itemOffered"]) : product;
        Json row{{"product_name", text(identity, "name")},
                 {"offer_type", aggregate ? "aggregate_range" : "offer"},
                 {"schema_path", path},
                 {"qualifiers", Json::array()},
                 {"conflicts", Json::array()}};
        field(row, value, "@id", "schema_id");
        for (const auto* key : {"sku", "gtin", "gtin8", "gtin12", "gtin13", "gtin14", "mpn", "model", "color",
                                "size", "itemCondition"})
            field(row, identity, key);
        row["brand"] = named(identity, "brand");
        if (identity.contains("additionalProperty") && identity["additionalProperty"].is_array()) {
            row["properties"] = Json::array();
            for (const auto& property : identity["additionalProperty"])
                if (row["properties"].size() < 12 && !text(property, "name").empty())
                    row["properties"].push_back(
                        Json{{"name", text(property, "name")}, {"value", text(property, "value")}});
        }
        for (const auto* key : {"sku", "itemCondition", "priceValidUntil", "validFrom", "validThrough",
                                "availabilityStarts", "availabilityEnds"})
            field(row, value, key);
        field(row, value, "name", "offer_name");
        row["seller"] = named(value, "seller");
        const auto raw_url = text(value, "url", 2048);
        row["url"] = safe_url(raw_url.empty() ? text(identity, "url", 2048) : raw_url, base);
        if (json_string(row, "url").empty()) {
            row["url"] = base;
            row["url_is_page_fallback"] = true;
        }
        row["availability"] = schema_name(text(value, "availability"));
        row["itemCondition"] = schema_name(json_string(row, "itemCondition"));
        const auto& specification =
            value.contains("priceSpecification") ? resolve(value["priceSpecification"]) : Json::object();
        const auto amount = price(value, "price");
        row["price"] = amount.empty() && specification.is_object() ? price(specification, "price") : amount;
        row["currency"] = text(value, "priceCurrency");
        if (json_string(row, "currency").empty() && specification.is_object())
            row["currency"] = text(specification, "priceCurrency");
        // Keep an explicit presentation-currency selection when a merchant's variant links omit it.
        try {
            const auto source = Url::parse(base), target = Url::parse(json_string(row, "url"));
            const auto wanted = "currency=" + json_string(row, "currency");
            const auto source_parts = split(source.query, '&', false);
            const auto target_parts = split(target.query, '&', false);
            if (same_product(base, json_string(row, "url")) &&
                std::count(source_parts.begin(), source_parts.end(), wanted) == 1 &&
                std::none_of(target_parts.begin(), target_parts.end(),
                             [](const auto& part) { return part.starts_with("currency="); })) {
                row["url_reported"] = row["url"];
                row["url"] = json_string(row, "url") + (target.has_query ? "&" : "?") + wanted;
                row["currency_context_preserved"] = true;
            }
        } catch (...) {
        }
        if (aggregate) {
            row["low_price"] = price(value, "lowPrice");
            row["high_price"] = price(value, "highPrice");
            row["qualifiers"].push_back("aggregate_or_from_price_not_exact_variant");
        }
        for (const auto* key : {"acceptedPaymentMethod", "eligibleCustomerType", "eligibleQuantity",
                                "eligibleRegion", "businessFunction", "addOn"})
            if (value.contains(key))
                row["qualifiers"].push_back(std::string(key) + ": " +
                                            evidence_excerpt(value[key].dump(), "", 400));
        if (specification.is_array())
            row["qualifiers"].push_back("multiple_price_specifications_require_review");
        if (specification.is_object()) {
            for (const auto* key :
                 {"billingDuration", "billingIncrement", "billingStart", "unitCode", "unitText", "priceType",
                  "priceComponentType", "referenceQuantity", "eligibleQuantity", "eligibleTransactionVolume"})
                if (specification.contains(key))
                    row["qualifiers"].push_back(std::string(key) + ": " +
                                                evidence_excerpt(specification[key].dump(), "", 400));
            const auto nested = price(specification, "price");
            if (!amount.empty() && !nested.empty() && amount != nested)
                row["conflicts"].push_back("price_specification_disagrees");
            const auto currency = text(specification, "priceCurrency");
            if (!currency.empty() && currency != json_string(row, "currency"))
                row["conflicts"].push_back("price_currency_disagrees");
        }
        enrich(row);
        // Duplicate references to one offer do not make additional evidence.
        auto fingerprint = row;
        fingerprint.erase("schema_path");
        if (!seen.insert(fingerprint.dump()).second)
            return;
        const auto size = row.dump().size();
        if (bytes + size > max_offer_bytes) {
            truncated = true;
            return;
        }
        bytes += size;
        records.push_back(std::move(row));
    }
    void walk(const Json& raw, std::string path, unsigned depth = 0, const Json& group = Json::object()) {
        if (depth > 16 || ++visited > 2048) {
            truncated = true;
            return;
        }
        const auto& value = resolve(raw);
        if (value.is_array()) {
            for (std::size_t i = 0; i < value.size(); ++i)
                walk(value[i], path + "[" + std::to_string(i) + "]", depth + 1, group);
        } else if (value.is_object()) {
            if (type_is(value, "Product") || type_is(value, "ProductGroup") ||
                type_is(value, "IndividualProduct")) {
                auto product = value;
                // Group identity is inherited, but SKU, condition and dimensions belong to the variant.
                for (const auto* key : {"name", "brand"})
                    if (!product.contains(key) && group.contains(key))
                        product[key] = group[key];
                if (product.contains("offers"))
                    offer(product["offers"], product, path + ".offers", depth + 1);
                if (product.contains("hasVariant"))
                    walk(product["hasVariant"], path + ".hasVariant", depth + 1, product);
            } else if (type_is(value, "Offer") || type_is(value, "AggregateOffer"))
                offer(value, Json::object(), path, depth + 1);
            for (const auto* key : {"@graph", "mainEntity", "itemListElement", "item"})
                if (value.contains(key))
                    walk(value[key], path + "." + key, depth + 1);
        }
    }
};
} // namespace

Json extract_offer_records(const Json& schemas, const Json& variants, std::string_view base_url) {
    Extractor extractor{std::string(base_url), variants};
    extractor.index(schemas);
    extractor.visited = 0;
    for (std::size_t i = 0; i < schemas.size(); ++i)
        extractor.walk(schemas[i], "jsonld[" + std::to_string(i) + "]$");
    std::set<std::string> contextual_ids;
    for (const auto& row : extractor.records)
        if (!json_string(row, "product_name").empty() && !json_string(row, "schema_id").empty())
            contextual_ids.insert(json_string(row, "schema_id"));
    for (auto row = extractor.records.begin(); row != extractor.records.end();)
        if (json_string(*row, "product_name").empty() &&
            contextual_ids.contains(json_string(*row, "schema_id")))
            row = extractor.records.erase(row);
        else
            ++row;
    return Json{{"records", extractor.records},
                {"truncated", extractor.truncated},
                {"ambiguous_reference_ids", extractor.duplicate_ids.size()}};
}

Json offer_view(const Json& document, std::size_t offset, std::size_t limit) {
    Json result{{"offers", Json::array()},
                {"offer_offset", offset},
                {"total_offers", 0},
                {"offers_truncated", json_bool(document, "offers_truncated")},
                {"checked_at", json_string(document, "validated_at", json_string(document, "retrieved_at"))},
                {"price_evidence", "none"}};
    if (!document.contains("offers") || !document["offers"].is_array() ||
        (json_string(document, "status") != "ok" && json_string(document, "status") != "insufficient_text"))
        return result;
    const auto& offers = document["offers"];
    result["total_offers"] = offers.size();
    const auto checked_at = parse_utc(json_string(result, "checked_at"));
    for (std::size_t i = std::min(offset, offers.size());
         i < offers.size() && result["offers"].size() < limit; ++i) {
        auto row = offers[i];
        row["offer_index"] = i;
        row["missing_fields"] = Json::array();
        for (const auto* key : {"product_name", "price", "currency", "availability", "itemCondition"})
            if (json_string(row, key).empty())
                row["missing_fields"].push_back(key);
        if (!RE2::FullMatch(json_string(row, "currency"), "[A-Z]{3}"))
            row["qualifiers"].push_back("currency_missing_or_invalid");
        for (const auto* key :
             {"priceValidUntil", "validThrough", "availabilityEnds", "validFrom", "availabilityStarts"}) {
            const bool starts =
                std::string_view(key) == "validFrom" || std::string_view(key) == "availabilityStarts";
            auto stamp = json_string(row, key);
            if (stamp.empty())
                continue;
            if (stamp.size() == 10)
                stamp += starts ? "T00:00:00Z" : "T23:59:59.999Z";
            const auto parsed = parse_utc(stamp);
            if (!parsed || !checked_at)
                row["qualifiers"].push_back(std::string(key) + "_requires_date_review");
            else if (starts ? *parsed > *checked_at : *parsed < *checked_at)
                row["qualifiers"].push_back(std::string(key) + (starts ? "_future" : "_expired"));
        }
        row["stock_status_reported"] = json_string(row, "availability");
        row["assessment"] = !row["conflicts"].empty()                             ? "conflicting_source_data"
                            : json_string(row, "offer_type") == "aggregate_range" ? "aggregate_only"
                            : json_string(row, "price").empty() || json_string(row, "product_name").empty()
                                ? "incomplete_offer"
                                : "source_reported_offer";
        result["offers"].push_back(std::move(row));
    }
    if (offset + result["offers"].size() < offers.size())
        result["next_offer_offset"] = offset + result["offers"].size();
    if (!offers.empty())
        result["price_evidence"] = "source_reported";
    result["offer_note"] =
        "Prices and stock are retailer-reported at checked_at, not a checkout guarantee. "
        "Keep currency, exact variant, payment/eligibility options and qualifiers together. "
        "Aggregate ranges are not exact offers. Missing condition, delivery, tax or UK "
        "eligibility stays unknown. Do not infer availability from boilerplate text.";
    return result;
}
} // namespace devbox::web
