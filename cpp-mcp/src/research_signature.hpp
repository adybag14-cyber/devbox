#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace devbox::web::detail {
struct TextSignature {
    std::vector<std::uint64_t> shingles;
    std::vector<std::string> significant_tokens;
    std::size_t words = 0;
    bool sampled = false;
};
TextSignature content_signature(std::string_view text);
double similarity(const TextSignature& left, const TextSignature& right);
} // namespace devbox::web::detail
