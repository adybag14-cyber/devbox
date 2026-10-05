#include "web_content.hpp"
#include "devbox/common.hpp"
#include <ada.h>
#include <algorithm>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/document.h>
#include <re2/re2.h>

namespace devbox::web::content {
namespace {
std::string attribute(lxb_dom_node_t* node, std::string_view name) {
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return {};
    std::size_t length = 0;
    const auto* value =
        lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                      reinterpret_cast<const lxb_char_t*>(name.data()), name.size(), &length);
    return value ? std::string(reinterpret_cast<const char*>(value), length) : "";
}
std::string short_text(lxb_dom_node_t* root, std::size_t limit = 512) {
    std::string text;
    std::vector<lxb_dom_node_t*> pending{root};
    unsigned visits = 0;
    while (!pending.empty() && text.size() < limit && ++visits <= 256) {
        auto* node = pending.back();
        pending.pop_back();
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            const auto& data = lxb_dom_interface_text(node)->char_data.data;
            text.append(reinterpret_cast<const char*>(data.data), std::min(data.length, limit - text.size()));
            text += ' ';
        }
        for (auto* child = node->last_child; child; child = child->prev)
            pending.push_back(child);
    }
    return trim(text);
}
bool related_heading(lxb_dom_node_t* node) {
    if (node->local_name != LXB_TAG_SECTION && node->local_name != LXB_TAG_DIV &&
        node->local_name != LXB_TAG_ASIDE)
        return false;
    static const RE2 label(
        "(?i)^\\s*(?:related (?:posts|articles|stories|content)|recommended(?: for you)?|"
        "you may also like|похожие (?:посты|статьи|новости)|читайте также|рекомендуем)\\s*[:.]?\\s*$");
    if (RE2::FullMatch(attribute(node, "aria-label"), label))
        return true;
    unsigned children = 0;
    for (auto* child = node->first_child; child && ++children <= 8; child = child->next)
        if ((child->local_name == LXB_TAG_H2 || child->local_name == LXB_TAG_H3 ||
             child->local_name == LXB_TAG_H4) &&
            RE2::FullMatch(short_text(child, 256), label))
            return true;
    return false;
}
bool self_link(lxb_dom_node_t* node, std::string_view url) {
    if (node->local_name != LXB_TAG_A)
        return false;
    bool permalink_context =
        attribute(node, "itemprop") == "url" || attribute(node, "rel").find("bookmark") != std::string::npos;
    if (node->parent)
        permalink_context = permalink_context || node->parent->local_name == LXB_TAG_H1 ||
                            node->parent->local_name == LXB_TAG_H2 || node->parent->local_name == LXB_TAG_H3;
    unsigned children = 0;
    for (auto* child = node->first_child; child && ++children <= 8; child = child->next)
        permalink_context = permalink_context || child->local_name == LXB_TAG_TIME;
    if (!permalink_context)
        return false;
    const auto parent = ada::parse<ada::url_aggregator>(url);
    auto link = ada::parse<ada::url_aggregator>(attribute(node, "href"), parent ? &*parent : nullptr);
    if (!parent || !link)
        return false;
    link->set_hash("");
    return link->get_href() == parent->get_href();
}
} // namespace
bool non_text_tag(std::uintptr_t tag) {
    return tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE || tag == LXB_TAG_TEMPLATE || tag == LXB_TAG_SVG ||
           tag == LXB_TAG_CANVAS || tag == LXB_TAG_NAV || tag == LXB_TAG_FOOTER || tag == LXB_TAG_FORM ||
           tag == LXB_TAG_SELECT || tag == LXB_TAG_OPTION || tag == LXB_TAG_BUTTON || tag == LXB_TAG_IFRAME ||
           tag == LXB_TAG_OBJECT || tag == LXB_TAG_EMBED;
}
bool excluded(lxb_dom_node_t* node) {
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                                      reinterpret_cast<const lxb_char_t*>("hidden"), 6) ||
        lower(trim(attribute(node, "aria-hidden"))) == "true")
        return true;
    static const RE2 hidden("(?i)(?:^|;)\\s*(?:display\\s*:\\s*none|visibility\\s*:\\s*(?:hidden|collapse)|"
                            "content-visibility\\s*:\\s*hidden)\\s*(?:!important\\s*)?(?:;|$)");
    if (RE2::PartialMatch(attribute(node, "style"), hidden))
        return true;
    const auto role = lower(trim(attribute(node, "role")));
    if (role == "navigation" || role == "banner" || role == "complementary" || role == "contentinfo")
        return true;
    static const RE2 publication("(?i)(?:^|[\\s_-])(?:published|updated|modified|timestamp)(?:$|[\\s_-])");
    if (node->local_name == LXB_TAG_TIME &&
        (attribute(node, "itemprop") == "datePublished" || attribute(node, "itemprop") == "dateModified" ||
         RE2::PartialMatch(attribute(node, "class"), publication)))
        return true;
    static const RE2 noise("(?i)(?:^|[\\s_-])(?:ads?|advert(?:isement|ising)?|adslot|related|recommendations|"
                           "recommended|sidebar|navigation|social-share|share-buttons|breadcrumbs?|comments)"
                           "(?:$|[\\s_-])");
    if (RE2::PartialMatch(attribute(node, "class") + " " + attribute(node, "id"), noise) ||
        node->local_name == LXB_TAG_ASIDE || related_heading(node))
        return true;
    if (node->local_name == LXB_TAG_NOSCRIPT) {
        static const RE2 executable("(?:window\\.|document\\.|yaContextCb|\\bfunction\\s*\\(|googletag\\.)");
        return RE2::PartialMatch(short_text(node, 2048), executable);
    }
    return false;
}
Selection select(lxb_dom_node_t* document, std::string_view url) {
    struct Node {
        lxb_dom_node_t* node;
        std::size_t parent, bytes = 0, linked_bytes = 0, paragraph_bytes = 0, links = 0;
        bool in_link = false, in_article = false, headline = false, permalink = false;
        bool heading_link = false, more_link = false;
    };
    constexpr auto none = static_cast<std::size_t>(-1);
    struct Pending {
        lxb_dom_node_t* node;
        std::size_t parent;
        bool in_link, in_article;
    };
    std::vector<Pending> pending{{document, none, false, false}};
    std::vector<Node> nodes;
    nodes.reserve(2048);
    Selection result;
    std::string title;
    std::size_t body = none, main = none;
    while (!pending.empty() && nodes.size() < 100000) {
        const auto item = pending.back();
        pending.pop_back();
        auto* node = item.node;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && (non_text_tag(node->local_name) || excluded(node))) {
            ++result.excluded_regions;
            continue;
        }
        const auto index = nodes.size();
        nodes.push_back({node, item.parent});
        auto& entry = nodes.back();
        entry.in_link = item.in_link || node->local_name == LXB_TAG_A;
        entry.in_article = item.in_article;
        entry.permalink = self_link(node, url);
        if (node->local_name == LXB_TAG_A) {
            entry.links = 1;
            if (!entry.permalink && !attribute(node, "href").empty()) {
                static const RE2 more(
                    "(?i)^\\s*(?:read more|continue reading|читать далее|подробнее)\\s*[.→…]*\\s*$");
                entry.more_link = RE2::FullMatch(short_text(node, 128), more);
                auto* parent = node->parent;
                for (unsigned depth = 0; parent && depth < 3; ++depth, parent = parent->parent)
                    if (parent->local_name == LXB_TAG_H1 || parent->local_name == LXB_TAG_H2 ||
                        parent->local_name == LXB_TAG_H3)
                        entry.heading_link = true;
            }
        }
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            entry.bytes = lxb_dom_interface_text(node)->char_data.data.length;
            entry.linked_bytes = entry.in_link ? entry.bytes : 0;
        }
        if (node->local_name == LXB_TAG_TITLE && title.empty())
            title = short_text(node);
        if (node->local_name == LXB_TAG_H1) {
            const auto heading = short_text(node);
            entry.headline = heading.size() >= 8 && !title.empty() &&
                             RE2::PartialMatch(title, RE2("(?i)" + RE2::QuoteMeta(heading)));
        }
        if (node->local_name == LXB_TAG_BODY)
            body = index;
        if (node->local_name == LXB_TAG_MAIN || lower(attribute(node, "role")) == "main")
            main = index;
        for (auto* child = node->last_child; child; child = child->prev)
            pending.push_back(
                {child, index, entry.in_link, item.in_article || node->local_name == LXB_TAG_ARTICLE});
    }
    result.truncated = !pending.empty();
    if (nodes.empty()) {
        result.root = document;
        result.warning = "no_visible_content_region";
        return result;
    }
    for (std::size_t i = nodes.size(); i-- > 0;) {
        auto& entry = nodes[i];
        if (entry.node->local_name == LXB_TAG_P)
            entry.paragraph_bytes = entry.bytes;
        if (entry.parent != none) {
            auto& parent = nodes[entry.parent];
            parent.bytes += entry.bytes;
            parent.linked_bytes += entry.linked_bytes;
            parent.paragraph_bytes += entry.paragraph_bytes;
            parent.links += entry.links;
            parent.headline = parent.headline || entry.headline;
            parent.permalink = parent.permalink || entry.permalink;
            parent.heading_link = parent.heading_link || entry.heading_link;
            parent.more_link = parent.more_link || entry.more_link;
        }
    }
    auto chosen = main != none ? main : body != none ? body : 0;
    result.method = main != none ? "semantic_main" : "body_fallback";
    std::vector<std::size_t> articles, identified_articles, bodies, identified_bodies;
    std::vector<bool> in_declared_body(nodes.size(), false);
    static const RE2 body_class("(?i)(?:^|\\s)(?:article|story|entry|post)[_-]+(?:body|content)(?:$|\\s)");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& entry = nodes[i];
        const bool nested_body = entry.parent != none && in_declared_body[entry.parent];
        in_declared_body[i] = nested_body;
        if (entry.node->type != LXB_DOM_NODE_TYPE_ELEMENT)
            continue;
        const auto itemprop = attribute(entry.node, "itemprop");
        const bool declared_body =
            itemprop == "articleBody" || attribute(entry.node, "data-tv-page-type") == "post" ||
            RE2::PartialMatch(attribute(entry.node, "class") + " " + attribute(entry.node, "id"), body_class);
        const bool article = entry.node->local_name == LXB_TAG_ARTICLE;
        in_declared_body[i] = declared_body || nested_body;
        const bool identified = entry.headline || (article && entry.permalink) ||
                                attribute(entry.node, "data-tv-page-type") == "post";
        if (article && !entry.in_article) {
            articles.push_back(i);
            if (identified)
                identified_articles.push_back(i);
        }
        if (declared_body && !nested_body) {
            bodies.push_back(i);
            if (identified)
                identified_bodies.push_back(i);
        }
    }
    bool unresolved = false;
    if (identified_articles.size() == 1 || articles.size() == 1) {
        chosen = identified_articles.size() == 1 ? identified_articles.front() : articles.front();
        result.method = "semantic_article";
        result.type = "article";
    } else if (!articles.empty())
        unresolved = true;
    else if (identified_bodies.size() == 1 || bodies.size() == 1) {
        chosen = identified_bodies.size() == 1 ? identified_bodies.front() : bodies.front();
        result.method = "declared_article_body";
        result.type = "article";
    } else if (!bodies.empty())
        unresolved = true;
    std::string path;
    if (const auto parsed = ada::parse<ada::url_aggregator>(url))
        path = std::string(parsed->get_pathname());
    static const RE2 index_path(
        "(?i)(?:^|/)(?:tags?|categor(?:y|ies)|topics?|search|archives?|authors?)(?:/|$)");
    const auto& selected = nodes[chosen];
    const bool known_index = RE2::PartialMatch(path, index_path);
    if (known_index || unresolved || (selected.links >= 6 && selected.linked_bytes * 2 > selected.bytes)) {
        chosen = main != none ? main : body != none ? body : chosen;
        result.type = "index";
        result.method = "collection";
        result.ambiguous = !known_index && unresolved;
        result.warning =
            result.ambiguous ? "multiple_article_regions_without_primary_identity" : "discovery_index";
    } else if (selected.paragraph_bytes < 400 && selected.heading_link && selected.more_link) {
        result.type = "teaser";
        result.warning = "teaser_links_to_full_document";
    }
    result.root = nodes[chosen].node;
    result.confidence = result.ambiguous || result.truncated || result.method == "body_fallback" ? "low"
                        : result.method == "semantic_main"                                       ? "medium"
                                                                                                 : "high";
    return result;
}
} // namespace devbox::web::content
