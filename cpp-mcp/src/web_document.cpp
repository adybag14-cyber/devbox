#include "devbox/native.hpp"
#include "devbox/research.hpp"
#include "devbox/web_retailers.hpp"
#include <algorithm>
#include <cctype>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/document.h>
#include <map>
#include <re2/re2.h>
#include <set>

namespace devbox::web {
namespace {
constexpr std::size_t text_limit = 65536;
std::string clipped(std::string_view text, std::size_t limit) {
    if (text.size() <= limit)
        return std::string(text);
    while (limit && (static_cast<unsigned char>(text[limit]) & 0xc0) == 0x80)
        --limit;
    return std::string(text.substr(0, limit));
}
void codepoint(std::string& output, unsigned value) {
    if (value < 128)
        output.push_back(static_cast<char>(value));
    else if (value < 2048) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 63)));
    } else {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 63)));
        output.push_back(static_cast<char>(0x80 | (value & 63)));
    }
}
std::string utf8(std::string_view input, std::string_view content_type) {
    const auto charset = lower(std::string(content_type));
    if (charset.find("windows-1252") != charset.npos || charset.find("iso-8859-1") != charset.npos) {
        static constexpr unsigned cp1252[32] = {
            0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
            0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd, 0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
            0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178};
        std::string output;
        output.reserve(input.size() * 2);
        for (const unsigned char byte : input)
            codepoint(output, byte >= 0x80 && byte <= 0x9f ? cp1252[byte - 0x80] : byte);
        return output;
    }
    // Do not silently turn an unknown encoding into corrupted evidence.
    (void)Json(std::string(input)).dump();
    return std::string(input);
}
std::string compact(std::string_view text, std::size_t limit = text_limit) {
    std::string result;
    bool space = false;
    for (const unsigned char c : text) {
        if (c < 128 && std::isspace(c)) {
            space = !result.empty();
            continue;
        }
        if (space && result.size() < limit)
            result += ' ';
        space = false;
        result += static_cast<char>(c);
        if (result.size() >= limit + 4)
            break;
    }
    return clipped(result, limit);
}
std::string attribute(lxb_dom_node_t* node, std::string_view name) {
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return {};
    std::size_t length = 0;
    auto* value =
        lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                      reinterpret_cast<const lxb_char_t*>(name.data()), name.size(), &length);
    return value ? std::string(reinterpret_cast<const char*>(value), length) : "";
}
bool skip_tag(std::uintptr_t tag);
std::string node_text(lxb_dom_node_t* root, std::size_t limit, bool raw = false) {
    std::string text;
    std::vector<lxb_dom_node_t*> stack{root};
    std::size_t visited = 0;
    while (!stack.empty() && text.size() < limit && ++visited <= 100000) {
        auto* node = stack.back();
        stack.pop_back();
        if (!raw && node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            (skip_tag(node->local_name) ||
             lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                                           reinterpret_cast<const lxb_char_t*>("hidden"), 6) ||
             lower(attribute(node, "aria-hidden")) == "true"))
            continue;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            const auto& value = lxb_dom_interface_text(node)->char_data.data;
            text += clipped(std::string_view(reinterpret_cast<const char*>(value.data), value.length),
                            limit - text.size());
            if (text.size() < limit)
                text += ' ';
        }
        for (auto* child = node->last_child; child; child = child->prev)
            stack.push_back(child);
    }
    return compact(text, limit);
}
bool skip_tag(std::uintptr_t tag) {
    return tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE || tag == LXB_TAG_TEMPLATE || tag == LXB_TAG_SVG ||
           tag == LXB_TAG_CANVAS || tag == LXB_TAG_NAV || tag == LXB_TAG_FOOTER || tag == LXB_TAG_HEADER ||
           tag == LXB_TAG_FORM || tag == LXB_TAG_SELECT || tag == LXB_TAG_OPTION || tag == LXB_TAG_BUTTON;
}
Json bounded_json(std::string_view input) {
    return Json::parse(input, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64)
            throw Error("WEB_JSON_DEPTH");
        return true;
    });
}
bool jsonld(const Json& value, Json& result) {
    struct Entry {
        const Json* value;
        unsigned depth;
        std::string path, name, url;
    };
    std::vector<Entry> stack{{&value, 0, "$", "", ""}};
    std::size_t visited = 0;
    bool truncated = false;
    while (!stack.empty() && result.size() < 64 && ++visited <= 512) {
        const auto entry = stack.back();
        const auto* current = entry.value;
        const auto depth = entry.depth;
        stack.pop_back();
        if (depth > 12) {
            truncated = true;
            continue;
        }
        if (current->is_array()) {
            for (std::size_t i = current->size(); i > 0 && stack.size() < 512; --i)
                stack.push_back({&current->at(i - 1), depth + 1,
                                 entry.path + "[" + std::to_string(i - 1) + "]", entry.name, entry.url});
        } else if (current->is_object()) {
            Json item = Json::object();
            for (const auto* key :
                 {"@type", "name", "headline", "description", "datePublished", "dateModified", "price",
                  "lowPrice", "highPrice", "priceCurrency", "availability", "sku", "url", "priceValidUntil",
                  "validFrom", "validThrough"}) {
                const auto found = current->find(key);
                if (found != current->end() &&
                    (found->is_string() || found->is_number() || found->is_boolean()))
                    item[key] = found->is_string() ? Json(clipped(found->get_ref<const std::string&>(), 2048))
                                                   : *found;
            }
            if (current->contains("@type") && current->at("@type").is_array()) {
                item["@type"] = Json::array();
                for (const auto& type : current->at("@type"))
                    if (type.is_string() && item["@type"].size() < 8)
                        item["@type"].push_back(clipped(type.get_ref<const std::string&>(), 100));
            }
            const auto name = json_string(*current, "name", json_string(*current, "headline", entry.name));
            const auto url = json_string(*current, "url", entry.url);
            auto kind = item.contains("@type") ? item["@type"].dump() : "";
            const bool useful = kind.find("Product") != kind.npos || kind.find("Offer") != kind.npos ||
                                kind.find("Article") != kind.npos || kind.find("WebPage") != kind.npos ||
                                kind.find("BlogPosting") != kind.npos || kind.find("ItemList") != kind.npos ||
                                kind.find("PriceSpecification") != kind.npos;
            item["schema_path"] = entry.path;
            if (!entry.name.empty())
                item["context_name"] = clipped(entry.name, 500);
            if (!entry.url.empty())
                item["context_url"] = clipped(entry.url, 2048);
            if (useful && item.contains("@type") && item.size() > 2) {
                if (result.dump().size() + item.dump().size() <= 32768)
                    result.push_back(std::move(item));
                else
                    truncated = true;
            }
            for (const auto* key : {"@graph", "offers", "mainEntity", "itemListElement", "item", "author",
                                    "brand", "priceSpecification"})
                if (current->contains(key))
                    stack.push_back({&current->at(key), depth + 1, entry.path + "." + key, name, url});
        }
    }
    return truncated || !stack.empty();
}
// Read inert, explicitly product-scoped variant data. Analytics and executable scripts are excluded.
bool variant_script(lxb_dom_node_t* script) {
    if (attribute(script, "data-element") == "variants-data")
        return true;
    auto* parent = script->parent;
    for (unsigned i = 0; parent && i < 4; ++i, parent = parent->parent)
        if (!attribute(parent, "data-url").empty())
            return true;
    return false;
}
void product_variants(lxb_dom_node_t* script, const Json& data, std::string_view base, Json& variants) {
    auto* scope = script->parent;
    std::string product_url;
    for (unsigned i = 0; scope && i < 4; ++i, scope = scope->parent)
        if (!attribute(scope, "data-url").empty()) {
            product_url = attribute(scope, "data-url");
            break;
        }
    const bool variant_map = attribute(script, "data-element") == "variants-data";
    if (product_url.empty() && !variant_map)
        return;
    try {
        product_url = normalize_url(product_url.empty() ? base : product_url, base);
    } catch (...) {
        return;
    }
    Json names = Json::array();
    if (scope && !variant_map) {
        std::vector<lxb_dom_node_t*> pending{scope};
        std::size_t count = 0;
        while (!pending.empty() && ++count < 5000 && names.size() < 8) {
            auto* node = pending.back();
            pending.pop_back();
            const auto name = attribute(node, "name");
            if (node->local_name == LXB_TAG_SELECT && name.starts_with("options[") && name.ends_with(']'))
                names.push_back(clipped(name.substr(8, name.size() - 9), 150));
            for (auto* child = node->last_child; child; child = child->prev)
                pending.push_back(child);
        }
    }
    const auto append = [&](const Json& item, std::string id) {
        if (!item.is_object() || variants.size() >= 256 || !item.contains("options") ||
            !item["options"].is_array() || item["options"].empty() || item["options"].size() > 8)
            return;
        if (id.empty() && item.contains("id"))
            id = item["id"].is_string() ? item["id"].get<std::string>() : item["id"].dump();
        if (!RE2::FullMatch(id, "[0-9]{1,24}"))
            return;
        Json options = Json::array();
        for (const auto& option : item["options"]) {
            if (!option.is_string())
                return;
            options.push_back(clipped(option.get<std::string>(), 200));
        }
        Json variant{{"id", id},
                     {"product_url", product_url},
                     {"options", options},
                     {"evidence_path", variant_map ? "script[data-element=variants-data]"
                                                   : "product-scoped script[type=application/json]"}};
        if (names.size() == options.size())
            variant["option_names"] = names;
        for (const auto* key : {"title", "sku"})
            if (item.contains(key) && item[key].is_string())
                variant[key] = clipped(item[key].get<std::string>(), 500);
        for (const auto* key : {"available", "requires_selling_plan"})
            if (item.contains(key) && item[key].is_boolean())
                variant[key] = item[key];
        variants.push_back(std::move(variant));
    };
    if (variant_map && data.is_object())
        for (const auto& entry : data.items())
            append(entry.value(), entry.key());
    else if (!variant_map && data.is_array())
        for (const auto& item : data)
            append(item, "");
}
} // namespace

bool response_requires_challenge(const Transfer& response) {
    const auto body = lower(response.body.substr(0, 128 * 1024));
    return (body.find("anomaly-modal") != body.npos && body.find("challenge-form") != body.npos &&
            body.find("anomaly.js") != body.npos) ||
           RE2::PartialMatch(body, RE2("<title[^>]*>\\s*(?:just a moment|attention required|access denied)"));
}

Json extract_document(const Transfer& response) {
    Json result{{"url", response.url},
                {"final_url", response.final_url.empty() ? response.url : response.final_url},
                {"http_status", response.status},
                {"retrieved_at", response.completed_at.empty() ? utc_now() : response.completed_at},
                {"http_last_modified", json_string(response.headers, "last-modified")},
                {"http_date", clipped(json_string(response.headers, "date"), 256)},
                {"http_age", clipped(json_string(response.headers, "age"), 256)},
                {"http_cache_control", clipped(json_string(response.headers, "cache-control"), 256)},
                {"etag", json_string(response.headers, "etag")},
                {"content_type", json_string(response.headers, "content-type")},
                {"decoded_bytes", response.bytes},
                {"duration_ms", response.duration_ms},
                {"request_attempts", response.attempts},
                {"status", "unavailable"},
                {"title", ""},
                {"text", ""},
                {"links", Json::array()},
                {"tables", Json::array()},
                {"headings", Json::array()},
                {"structured_metadata", Json::array()},
                {"structured_metadata_truncated", false},
                {"published_at_reported", ""},
                {"modified_at_reported", ""},
                {"content_truncated", false},
                {"untrusted_source", true}};
    if (!response.error.empty()) {
        result["error"] = response.error;
        return result;
    }
    if (response_requires_challenge(response)) {
        result["status"] = "challenge_required";
        result["error"] = "Source requires human verification; no challenge was attempted";
        return result;
    }
    if (response.status == 429 || response.status == 503) {
        result["status"] = "rate_limited";
        result["retry_after"] = json_string(response.headers, "retry-after");
        return result;
    }
    if (response.status == 401 || response.status == 403) {
        result["status"] = "access_blocked";
        return result;
    }
    if (response.status < 200 || response.status >= 300) {
        result["error"] = "HTTP_" + std::to_string(response.status);
        return result;
    }
    auto type = lower(json_string(response.headers, "content-type"));
    if (type.find("pdf") != type.npos || response.body.starts_with("%PDF")) {
        result["status"] = "unsupported_pdf";
        return result;
    }
    if (!type.empty() && type.find("text/") != 0 && type.find("json") == type.npos &&
        type.find("xml") == type.npos && type.find("html") == type.npos) {
        result["status"] = "unsupported_content_type";
        return result;
    }
    std::string encoding;
    const RE2 charset("(?i)charset\\s*=\\s*[\"']?\\s*([a-z0-9_-]+)");
    RE2::PartialMatch(type, charset, &encoding);
    if (encoding.empty() && (type.find("html") != type.npos || type.empty()))
        RE2::PartialMatch(response.body.substr(0, 8192),
                          RE2("(?i)<meta[^>]{0,1000}charset\\s*=\\s*[\"']?\\s*([a-z0-9_-]+)"), &encoding);
    encoding = lower(encoding);
    if (encoding == "utf8")
        encoding = "utf-8";
    if (encoding == "cp1252")
        encoding = "windows-1252";
    if (encoding == "latin1" || encoding == "iso8859-1")
        encoding = "iso-8859-1";
    result["encoding_reported"] = encoding;
    if (!encoding.empty() && encoding != "utf-8" && encoding != "us-ascii" && encoding != "windows-1252" &&
        encoding != "iso-8859-1") {
        result["status"] = "unsupported_encoding";
        return result;
    }
    std::string body;
    try {
        body = utf8(response.body, encoding);
    } catch (...) {
        result["status"] = "unsupported_encoding";
        return result;
    }
    std::string text;
    Json schemas = Json::array(), variants = Json::array();
    std::size_t structured_bytes = 0;
    const bool html = type.find("html") != type.npos || (type.empty() && body.find('<') != body.npos);
    if (html) {
        auto* document = lxb_html_document_create();
        if (!document)
            throw Error("Could not allocate HTML document");
        ScopeExit destroy([&] { lxb_html_document_destroy(document); });
        if (lxb_html_document_parse(document, reinterpret_cast<const lxb_char_t*>(body.data()),
                                    body.size()) != LXB_STATUS_OK)
            throw Error("Native HTML parsing failed");
        auto* root = lxb_dom_interface_node(document);
        std::vector<lxb_dom_node_t*> stack{root};
        std::size_t visited = 0;
        std::size_t table_chars = 0;
        std::set<std::string> seen_links;
        std::map<lxb_dom_node_t*, std::size_t> table_indexes;
        while (!stack.empty() && ++visited <= 100000) {
            auto* node = stack.back();
            stack.pop_back();
            if (node->type == LXB_DOM_NODE_TYPE_TEXT && text.size() < text_limit) {
                const auto& value = lxb_dom_interface_text(node)->char_data.data;
                text += clipped(std::string_view(reinterpret_cast<const char*>(value.data), value.length),
                                text_limit - text.size());
                if (text.size() < text_limit)
                    text += ' ';
            }
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                const auto tag = node->local_name;
                if (tag == LXB_TAG_TITLE) {
                    result["title"] = node_text(node, 500);
                    continue;
                }
                if (tag == LXB_TAG_SCRIPT) {
                    if (lower(attribute(node, "type")) == "application/ld+json") {
                        try {
                            const auto raw = node_text(node, 65536, true);
                            const auto data = bounded_json(raw);
                            const auto truncated = jsonld(data, result["structured_metadata"]);
                            result["structured_metadata_truncated"] =
                                json_bool(result, "structured_metadata_truncated") || truncated;
                            if (structured_bytes + raw.size() <= 512 * 1024 && schemas.size() < 32) {
                                schemas.push_back(data);
                                structured_bytes += raw.size();
                            } else
                                result["offers_truncated"] = true;
                        } catch (...) {
                            result["structured_metadata_parse_warning"] = true;
                        }
                    } else if (lower(attribute(node, "type")) == "application/json" && variant_script(node)) {
                        if (structured_bytes >= 512 * 1024) {
                            result["variant_metadata_parse_warning"] = true;
                            continue;
                        }
                        try {
                            const auto raw = node_text(
                                node, std::min<std::size_t>(256 * 1024, 512 * 1024 - structured_bytes), true);
                            structured_bytes += raw.size();
                            product_variants(node, bounded_json(raw), json_string(result, "final_url"),
                                             variants);
                        } catch (...) {
                            result["variant_metadata_parse_warning"] = true;
                        }
                    }
                    continue;
                }
                if (tag == LXB_TAG_META) {
                    auto key = lower(attribute(node, "property"));
                    if (key.empty())
                        key = lower(attribute(node, "name"));
                    const auto value = clipped(attribute(node, "content"), 2048);
                    if (key == "article:published_time" || key == "datepublished")
                        result["published_at_reported"] = value;
                    if (key == "article:modified_time" || key == "datemodified")
                        result["modified_at_reported"] = value;
                    if (key == "og:title" && json_string(result, "title").empty())
                        result["title"] = value;
                    if (key == "description" || key == "og:description")
                        result["description_reported"] = value;
                }
                if (tag == LXB_TAG_LINK && lower(attribute(node, "rel")) == "canonical") {
                    try {
                        result["canonical_url_reported"] =
                            normalize_url(attribute(node, "href"), json_string(result, "final_url"));
                    } catch (...) {
                        result["structured_metadata_parse_warning"] = true;
                    }
                }
                if (skip_tag(tag) ||
                    lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                                                  reinterpret_cast<const lxb_char_t*>("hidden"), 6) ||
                    lower(attribute(node, "aria-hidden")) == "true")
                    continue;
                if (tag == LXB_TAG_DIV) {
                    static const RE2 empty_search_class("(?:^|\\s)no-results__message(?:$|\\s)");
                    if (RE2::PartialMatch(attribute(node, "class"), empty_search_class) &&
                        !node_text(node, 500).empty())
                        result["search_no_results"] = true;
                }
                if ((tag == LXB_TAG_H1 || tag == LXB_TAG_H2 || tag == LXB_TAG_H3 || tag == LXB_TAG_H4 ||
                     tag == LXB_TAG_H5 || tag == LXB_TAG_H6) &&
                    result["headings"].size() < 24)
                    result["headings"].push_back(node_text(node, 500));
                if (tag == LXB_TAG_A && result["links"].size() < 128) {
                    try {
                        auto url = normalize_url(attribute(node, "href"), json_string(result, "final_url"));
                        if (url.size() <= 2048 && seen_links.insert(url).second)
                            result["links"].push_back(Json{{"url", url},
                                                           {"text", node_text(node, 300)},
                                                           {"class", clipped(attribute(node, "class"), 200)},
                                                           {"rel", clipped(attribute(node, "rel"), 100)}});
                    } catch (...) {
                    }
                }
                if (tag == LXB_TAG_TABLE && result["tables"].size() < 8) {
                    table_indexes[node] = result["tables"].size();
                    result["tables"].push_back(Json::array());
                }
                if (tag == LXB_TAG_TR) {
                    auto* parent = node->parent;
                    while (parent && parent->local_name != LXB_TAG_TABLE)
                        parent = parent->parent;
                    if (parent && table_indexes.contains(parent)) {
                        auto& table = result["tables"][table_indexes[parent]];
                        if (table.size() < 40) {
                            Json row = Json::array();
                            for (auto* cell = node->first_child; cell && row.size() < 12; cell = cell->next)
                                if (cell->local_name == LXB_TAG_TD || cell->local_name == LXB_TAG_TH)
                                    row.push_back(node_text(cell, 512));
                            const auto row_chars = row.dump().size();
                            if (!row.empty() && table_chars + row_chars <= 32768) {
                                table_chars += row_chars;
                                table.push_back(std::move(row));
                            } else if (!row.empty())
                                result["tables_truncated"] = true;
                        }
                    }
                }
            }
            for (auto* child = node->last_child; child; child = child->prev)
                stack.push_back(child);
        }
        result["content_truncated"] = visited > 100000 || text.size() >= text_limit;
        text = compact(text);
    } else {
        text = clipped(body, text_limit);
        result["content_truncated"] = body.size() > text_limit;
        if (type.find("json") != type.npos) {
            try {
                const auto data = bounded_json(body);
                result["structured_metadata_truncated"] = jsonld(data, result["structured_metadata"]);
                schemas.push_back(data);
                const auto encoded = data.dump(1, ' ', false, Json::error_handler_t::replace);
                result["content_truncated"] =
                    json_bool(result, "content_truncated") || encoded.size() > text_limit;
                text = clipped(encoded, text_limit);
            } catch (...) {
                result["parse_warning"] = "Structured data was not valid bounded JSON";
            }
        }
    }
    for (const auto& item : result["structured_metadata"]) {
        const auto kind = item.contains("@type") ? item["@type"].dump() : "";
        if (kind.find("Article") == kind.npos && kind.find("WebPage") == kind.npos &&
            kind.find("BlogPosting") == kind.npos)
            continue;
        bool document_scope = json_string(item, "schema_path") == "$" && json_string(item, "url").empty();
        if (!json_string(item, "url").empty()) {
            try {
                document_scope = normalize_url(json_string(item, "url"), json_string(result, "final_url")) ==
                                 normalize_url(json_string(result, "final_url"));
            } catch (...) {
            }
        }
        if (!document_scope)
            continue;
        if (json_string(result, "published_at_reported").empty() && item.contains("datePublished"))
            result["published_at_reported"] = item["datePublished"];
        if (json_string(result, "modified_at_reported").empty() && item.contains("dateModified"))
            result["modified_at_reported"] = item["dateModified"];
    }
    const auto title = lower(json_string(result, "title"));
    const auto beginning = lower(text.substr(0, 3000));
    const bool challenge = title.find("just a moment") != title.npos ||
                           title.find("access denied") != title.npos ||
                           title.find("attention required") != title.npos ||
                           (text.size() < 3000 && (beginning.find("verify you are human") != beginning.npos ||
                                                   beginning.find("unusual traffic") != beginning.npos)) ||
                           response_requires_challenge(response);
    result["text"] = text;
    result["body_sha256"] = sha256(response.body);
    result["content_sha256"] = sha256(text);
    result["status"] = challenge ? "challenge_required" : text.size() < 120 ? "insufficient_text" : "ok";
    result["source_kind"] = html ? "html" : type.find("json") != type.npos ? "structured_data" : "text";
    const auto offers = extract_offer_records(schemas, variants, json_string(result, "final_url"));
    result["offers"] = offers["records"];
    if (result["offers"].empty() && !challenge)
        result["offers"] = retailer_offer_records(result);
    result["offers_truncated"] = json_bool(result, "offers_truncated") || json_bool(offers, "truncated");
    result["ambiguous_offer_reference_ids"] = offers["ambiguous_reference_ids"];
    return result;
}

std::string evidence_excerpt(std::string_view text, std::string_view query, std::size_t max_chars) {
    if (text.size() <= max_chars)
        return std::string(text);
    const auto haystack = lower(std::string(text));
    std::size_t best = text.npos;
    for (const auto& word : split(lower(std::string(query)), ' ', false)) {
        if (word.size() < 3)
            continue;
        const auto found = haystack.find(word);
        if (found != haystack.npos)
            best = std::min(best, found);
    }
    std::size_t begin = best == text.npos || best < max_chars / 4 ? 0 : best - max_chars / 4;
    while (begin && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80)
        --begin;
    return clipped(text.substr(begin), max_chars);
}

bool robots_allowed(std::string_view robots, std::string_view path, std::string_view agent) {
    struct Group {
        std::vector<std::string> agents;
        std::vector<std::pair<bool, std::string>> rules;
    };
    std::vector<Group> groups;
    Group current;
    for (const auto& raw : split(robots, '\n')) {
        const auto line = trim(std::string_view(raw).substr(0, raw.find('#')));
        const auto colon = line.find(':');
        if (colon == line.npos)
            continue;
        const auto key = lower(trim(std::string_view(line).substr(0, colon)));
        const auto value = trim(std::string_view(line).substr(colon + 1));
        if (key == "user-agent") {
            if (!current.rules.empty()) {
                groups.push_back(std::move(current));
                current = {};
            }
            current.agents.push_back(lower(value));
        } else if ((key == "allow" || key == "disallow") && !current.agents.empty() && value.size() <= 2048 &&
                   !value.empty())
            current.rules.emplace_back(key == "allow", value);
    }
    if (!current.agents.empty())
        groups.push_back(std::move(current));
    std::size_t specificity = 0;
    for (const auto& group : groups)
        for (const auto& value : group.agents)
            if (value != "*" && !value.empty() && lower(std::string(agent)).find(value) != std::string::npos)
                specificity = std::max(specificity, value.size());
    std::size_t longest = 0;
    bool allowed = true;
    for (const auto& group : groups) {
        bool selected = false;
        for (const auto& value : group.agents)
            selected |= specificity ? value.size() == specificity &&
                                          lower(std::string(agent)).find(value) != std::string::npos
                                    : value == "*";
        if (!selected)
            continue;
        for (const auto& [allow, rule] : group.rules) {
            std::string pattern = "^";
            const bool end = rule.ends_with('$');
            const auto body = end ? rule.substr(0, rule.size() - 1) : rule;
            for (std::size_t i = 0; i < body.size(); ++i)
                pattern += body[i] == '*' ? ".*" : RE2::QuoteMeta(std::string(1, body[i]));
            if (end)
                pattern += '$';
            if (rule.size() >= longest && RE2::PartialMatch(std::string(path), RE2(pattern))) {
                if (rule.size() > longest || allow)
                    allowed = allow;
                longest = rule.size();
            }
        }
    }
    return allowed;
}
} // namespace devbox::web
