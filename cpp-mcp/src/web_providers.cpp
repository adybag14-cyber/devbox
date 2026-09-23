#include "devbox/research.hpp"
#include <array>
#include <pugixml.hpp>
#include <re2/re2.h>

namespace devbox::web {
namespace {
using AddSource = std::function<void(const std::string&)>;
Json provider_json(const Transfer& response) {
    return Json::parse(response.body, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64)
            throw Error("WEB_JSON_DEPTH");
        return true;
    });
}
void duckduckgo(const Transfer& response, const AddSource& add, Json& result) {
    const auto document = extract_document(response);
    if (json_string(document, "status") == "challenge_required") {
        result["status"] = "challenge_required";
        result["error"] = "Search provider requires human verification; automatic requests stopped";
        return;
    }
    for (const auto& link : document.value("links", Json::array())) {
        static const RE2 result_class("(?:^|\\s)result__a(?:$|\\s)");
        if (!RE2::PartialMatch(json_string(link, "class"), result_class))
            continue;
        auto target = json_string(link, "url");
        const auto parameters = query_parameters(Url::parse(target).query);
        if (parameters.contains("uddg"))
            target = json_string(parameters, "uddg");
        add(target);
    }
    if (result["urls"].empty() && !json_bool(document, "search_no_results"))
        throw Error("Search HTML did not contain recognized result or no-result markup");
}
void bing(const Transfer& response, const AddSource& add, Json&) {
    pugi::xml_document document;
    if (!document.load_buffer(response.body.data(), response.body.size(),
                              pugi::parse_default | pugi::parse_doctype))
        throw Error("Malformed search RSS");
    for (const auto& node : document.children())
        if (node.type() == pugi::node_doctype)
            throw Error("Search RSS document types are not supported");
    const auto channel = document.child("rss").child("channel");
    if (!channel)
        throw Error("Search response did not contain an RSS channel");
    for (const auto& item : channel.children("item"))
        add(trim(item.child_value("link")));
    // Feed snippets and dates are discovery metadata, never fetched source evidence.
}
void crossref(const Transfer& response, const AddSource& add, Json&) {
    const auto data = provider_json(response);
    const auto& items = data.at("message").at("items");
    if (!items.is_array())
        throw Error("Crossref items must be an array");
    for (const auto& item : items) {
        auto url = json_string(item, "URL");
        if (item.contains("resource") && item["resource"].contains("primary"))
            url = json_string(item["resource"]["primary"], "URL", url);
        add(url);
    }
}
void mediawiki(const Transfer& response, const AddSource& add, Json&) {
    const auto data = provider_json(response);
    const auto& items = data.at("query").at("search");
    if (!items.is_array())
        throw Error("MediaWiki search must be an array");
    for (const auto& item : items) {
        const auto title = json_string(item, "title");
        if (title.empty())
            throw Error("MediaWiki search result has no title");
        add("https://en.wikipedia.org/wiki/" + url_encode(replace_all(title, " ", "_")));
    }
}
const std::array<DiscoveryProvider, 4>& providers() {
    static const std::array<DiscoveryProvider, 4> values{
        {{"duckduckgo_html",
          "General web; public HTML results may be incomplete or require human verification",
          "https://duckduckgo.com/terms",
          "Public HTML interface, not a guaranteed search API; respect service terms and access controls.",
          [](std::string_view q) { return "https://html.duckduckgo.com/html/?q=" + url_encode(q); },
          duckduckgo},
         {"bing_rss", "General web; a bounded RSS result set, not exhaustive coverage",
          "https://www.microsoft.com/en-us/servicesagreement",
          "Bing RSS is provided for personal, non-commercial aggregation. Preserve attribution; other uses "
          "require Microsoft's permission. This public interface is not a guaranteed search API.",
          [](std::string_view q) { return "https://www.bing.com/search?format=rss&q=" + url_encode(q); },
          bing},
         {"crossref", "Scholarly metadata registered with Crossref; source full text is retrieved separately",
          "https://www.crossref.org/documentation/retrieve-metadata/rest-api/",
          "Public metadata API. Respect rate limits and attribution; abstracts and linked full text may have "
          "separate rights.",
          [](std::string_view q) {
              return "https://api.crossref.org/"
                     "works?rows=100&select=DOI,title,URL,resource,published&query=" +
                     url_encode(q);
          },
          crossref},
         {"mediawiki", "English Wikipedia article search; secondary reference coverage",
          "https://www.mediawiki.org/wiki/API:Etiquette",
          "Respect API etiquette, rate limits and the source page's attribution and licensing requirements.",
          [](std::string_view q) {
              return "https://en.wikipedia.org/w/"
                     "api.php?action=query&list=search&srlimit=100&format=json&srsearch=" +
                     url_encode(q);
          },
          mediawiki}}};
    return values;
}
} // namespace
const DiscoveryProvider& discovery_provider(std::string_view id) {
    for (const auto& provider : providers())
        if (provider.id == id)
            return provider;
    throw Error("Unknown search provider");
}
Json discovery_provider_catalog() {
    Json result = Json::array();
    for (const auto& provider : providers())
        result.push_back(Json{{"id", provider.id},
                              {"coverage", provider.coverage},
                              {"terms_url", provider.terms_url},
                              {"use_notice", provider.use_notice},
                              {"keyless", true},
                              {"search_metadata_is_source_evidence", false}});
    return result;
}
} // namespace devbox::web
