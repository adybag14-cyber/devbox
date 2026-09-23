#include "devbox/web_retailers.hpp"
#include "devbox/native.hpp"
#include "devbox/research.hpp"
#include <algorithm>
#include <re2/re2.h>
#include <set>
namespace devbox::web {
namespace {
std::string amazon_asin(const Url& url) {
    static const std::set<std::string> hosts{
        "amazon.co.uk", "www.amazon.co.uk", "amazon.com",    "www.amazon.com",   "amazon.de", "www.amazon.de",
        "amazon.fr",    "www.amazon.fr",    "amazon.it",     "www.amazon.it",    "amazon.es", "www.amazon.es",
        "amazon.ca",    "www.amazon.ca",    "amazon.com.au", "www.amazon.com.au"};
    if (!hosts.contains(lower(url.host)))
        return {};
    std::string asin;
    static const RE2 pattern("/(?:dp|gp/product)/([A-Z0-9]{10})(?:/|$)");
    return RE2::PartialMatch(url.path, pattern, &asin) ? asin : "";
}
std::optional<Json> object_at(std::string_view text, std::size_t start) {
    bool quoted = false, escaped = false;
    int depth = 0;
    for (auto end = start; end < text.size() && end - start < 16384; ++end) {
        const auto c = text[end];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 16)
                return {};
        } else if (c == '}' || c == ']') {
            if (--depth == 0) {
                try {
                    std::size_t nodes = 0;
                    return Json::parse(text.substr(start, end - start + 1),
                                       [&](int d, Json::parse_event_t, Json&) {
                                           if (d > 16 || ++nodes > 2048)
                                               throw Error("RETAILER_JSON_BUDGET");
                                           return true;
                                       });
                } catch (...) {
                    return {};
                }
            }
        }
    }
    return {};
}
std::string decimal(const Json& value) {
    const auto raw = value.is_number() ? value.dump() : value.is_string() ? value.get<std::string>() : "";
    std::string whole, fraction;
    static const RE2 number("([0-9]{1,12})(?:\\.([0-9]{1,2}))?");
    if (!RE2::FullMatch(raw, number, &whole, &fraction))
        return {};
    while (whole.size() > 1 && whole[0] == '0')
        whole.erase(0, 1);
    fraction.resize(2, '0');
    return whole + "." + fraction;
}
std::string color_key(std::string value) {
    value = lower(value);
    std::string result;
    for (unsigned char c : value)
        if (c >= 'a' && c <= 'z')
            result += static_cast<char>(c);
    if (result == "blackblue" || result == "blueblack" || result == "blackandblue")
        return "blueblack";
    if (result == "grey")
        return "gray";
    return result;
}
std::string identity_text(std::string_view value) {
    std::string text;
    bool prior_digit = false, prior_letter = false;
    for (unsigned char raw : value) {
        const char c = static_cast<char>(raw >= 'A' && raw <= 'Z' ? raw + ('a' - 'A') : raw);
        const bool digit = c >= '0' && c <= '9', letter = (c >= 'a' && c <= 'z') || raw >= 128;
        if ((digit && prior_letter) || (letter && prior_digit))
            text += ' ';
        if (digit || letter || c == '+')
            text += c;
        else if (!text.empty() && text.back() != ' ')
            text += ' ';
        prior_digit = digit;
        prior_letter = letter;
    }
    return " " + trim(text) + " ";
}
} // namespace
Json match_product_targets(const Json& document, const Json& targets, bool require_offers) {
    Json matches = Json::array();
    for (const auto& target : targets) {
        std::vector<std::string> names;
        if (!require_offers || !json_bool(target, "require_offer"))
            names.push_back(json_string(document, "title"));
        if (document.contains("offers") && document["offers"].is_array())
            for (const auto& offer : document["offers"])
                names.push_back(json_string(offer, "product_name"));
        for (const auto& name : names) {
            const auto scope = identity_text(name);
            bool included = true;
            for (const auto& term : json_strings(target, "must_include"))
                included = included && scope.find(identity_text(term)) != std::string::npos;
            for (const auto& term : json_strings(target, "must_exclude"))
                if (scope.find(identity_text(term)) != std::string::npos)
                    included = false;
            if (included) {
                matches.push_back(json_string(target, "label"));
                break;
            }
        }
    }
    return matches;
}
std::string source_identity_url(std::string_view value) {
    try {
        const auto parsed = Url::parse(value);
        const auto asin = amazon_asin(parsed);
        if (!asin.empty()) {
            auto host = lower(parsed.host);
            if (!host.starts_with("www."))
                host = "www." + host;
            return "https://" + host + "/dp/" + asin;
        }
    } catch (...) {
    }
    return std::string(value);
}
Json retailer_offer_records(const Json& document) {
    Json result = Json::array();
    const auto url = json_string(document, "final_url", json_string(document, "url"));
    std::string asin;
    try {
        asin = amazon_asin(Url::parse(url));
    } catch (...) {
        return result;
    }
    if (asin.empty())
        return result;
    const auto found_text = document.find("text");
    if (found_text == document.end() || !found_text->is_string())
        return result;
    const auto& text = found_text->get_ref<const std::string&>();
    if (text.size() > 65536)
        return result;
    std::size_t position = 0, inspected = 0;
    std::set<std::string> seen;
    while ((position = text.find("{\"desktop_buybox_group_1\"", position)) != std::string::npos &&
           ++inspected <= 8) {
        const auto primary = object_at(text, position);
        const auto at = position++;
        if (!primary || !primary->contains("desktop_buybox_group_1") ||
            !primary->at("desktop_buybox_group_1").is_array())
            continue;
        // The companion buying-option map must agree on amount and condition. It is not a generic
        // currency regex, a trade-in saving, an unrelated recommendation, or a cached fallback.
        const auto nearby = std::string_view(text).substr(at, std::min<std::size_t>(8192, text.size() - at));
        std::vector<Json> companions;
        std::size_t seek = 0, attempts = 0;
        while ((seek = nearby.find("{\"0\":", seek)) != std::string_view::npos && companions.size() < 8 &&
               ++attempts <= 16) {
            if (auto data = object_at(nearby, seek))
                companions.push_back(std::move(*data));
            ++seek;
        }
        std::size_t options = 0;
        for (const auto& option : primary->at("desktop_buybox_group_1")) {
            if (++options > 16 || result.size() >= 16)
                break;
            if (!option.is_object() || !option.contains("priceAmount") ||
                !option.contains("aapiBuyingOptionIndex") ||
                !option["aapiBuyingOptionIndex"].is_number_unsigned())
                continue;
            const auto amount = decimal(option["priceAmount"]);
            const auto kind = json_string(option, "buyingOptionType");
            if (amount.empty() || (kind != "NEW" && kind != "USED"))
                continue;
            const auto index = json_uint(option, "aapiBuyingOptionIndex");
            if (index > 32)
                continue;
            for (const auto& companion : companions) {
                const auto key = std::to_string(index);
                if (!companion.is_object() || !companion.contains(key) || !companion[key].is_object())
                    continue;
                const auto& evidence = companion[key];
                if (json_string(evidence, "buyingOptionType") != kind || !evidence.contains("price") ||
                    !evidence["price"].is_object())
                    continue;
                const auto& price = evidence["price"];
                const auto currency = json_string(price, "currencyCode");
                auto primary_currency = json_string(option, "currencyCode");
                if (primary_currency.empty()) {
                    const auto symbol = json_string(option, "currencySymbol");
                    if (symbol == "\xc2\xa3")
                        primary_currency = "GBP";
                    else if (symbol == "\xe2\x82\xac")
                        primary_currency = "EUR";
                    else if (symbol == "US$")
                        primary_currency = "USD";
                    else if (symbol == "CA$")
                        primary_currency = "CAD";
                    else if (symbol == "AU$")
                        primary_currency = "AUD";
                }
                if (!primary_currency.empty() && currency != primary_currency)
                    continue;
                if (!price.contains("amount") || decimal(price["amount"]) != amount ||
                    !RE2::FullMatch(currency, "[A-Z]{3}"))
                    continue;
                Json row{
                    {"product_name", evidence_excerpt(json_string(document, "title"), "", 500)},
                    {"offer_type", "offer"},
                    {"price", amount},
                    {"currency", currency},
                    {"itemCondition", kind == "NEW" ? "NewCondition" : "UsedCondition"},
                    {"sku", asin},
                    {"asin", asin},
                    {"url", source_identity_url(url)},
                    {"schema_path", "amazon.buybox.desktop_buybox_group_1[" + std::to_string(index) + "]"},
                    {"extraction_method", "matched_inert_buybox_json"},
                    {"qualifiers", Json::array()},
                    {"conflicts", Json::array()}};
                if (evidence.contains("isAvailable") && evidence["isAvailable"].is_boolean())
                    row["availability"] = evidence["isAvailable"] == true ? "InStock" : "OutOfStock";
                const auto merchant = json_string(evidence, "encryptedMerchantId");
                if (RE2::FullMatch(merchant, "[A-Z0-9]{8,32}"))
                    row["merchant_id_reported"] = merchant;
                annotate_offer_consistency(row);
                if (seen.insert(row.dump()).second)
                    result.push_back(std::move(row));
            }
        }
    }
    return result;
}
void annotate_offer_consistency(Json& row) {
    if (!row.contains("conflicts"))
        row["conflicts"] = Json::array();
    const auto name = lower(json_string(row, "product_name")), color = color_key(json_string(row, "color"));
    static const std::vector<std::string> colors{"blueblack", "navy", "ultramarine", "black", "white",
                                                 "pink",      "teal", "silver",      "gray",  "grey",
                                                 "green",     "red",  "blue",        "gold"};
    const auto words = split(name, ' ', false);
    std::set<std::string> named_colors;
    for (const auto& word : words)
        if (std::find(colors.begin(), colors.end(), color_key(word)) != colors.end())
            named_colors.insert(color_key(word));
    if (!color.empty() && !named_colors.empty() && !named_colors.contains(color) &&
        !(color.find("silver") != color.npos && named_colors.contains("silver")) &&
        !(color.find("gray") != color.npos && named_colors.contains("gray")))
        row["conflicts"].push_back("product_name_color_disagrees");
    try {
        const auto url = Url::parse(json_string(row, "url"));
        std::set<std::string> segments;
        for (const auto& segment : split(url.path, '/', false))
            if (segment.size() > 12 && !segments.insert(segment).second) {
                row["conflicts"].push_back("reported_url_repeats_path_segments");
                break;
            }
    } catch (...) {
    }
}
} // namespace devbox::web
