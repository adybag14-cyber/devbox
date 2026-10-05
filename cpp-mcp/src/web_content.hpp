#pragma once
#include <cstddef>
#include <cstdint>
#include <lexbor/dom/interfaces/node.h>
#include <string>
#include <string_view>

namespace devbox::web::content {
struct Selection {
    lxb_dom_node_t* root = nullptr;
    std::string type = "document", method = "body_fallback", confidence = "low", warning;
    std::size_t excluded_regions = 0;
    bool ambiguous = false, truncated = false;
};
bool excluded(lxb_dom_node_t* node);
bool non_text_tag(std::uintptr_t tag);
Selection select(lxb_dom_node_t* document, std::string_view url);
} // namespace devbox::web::content
