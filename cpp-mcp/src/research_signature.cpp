#include "research_signature.hpp"
#include "devbox/common.hpp"
#include <algorithm>
#include <array>
#include <re2/re2.h>

namespace devbox::web::detail {
TextSignature content_signature(std::string_view text) {
    TextSignature result;
    std::array<std::string, 5> window;
    std::size_t next = 0;
    static const RE2 token("([\\p{L}\\p{N}][\\p{L}\\p{N}_-]*)");
    static const RE2 significant("(?i)(?:.*\\p{N}.*|no|not|never|without|нет|не|ни|никогда|без)");
    re2::StringPiece remaining(text.data(), text.size());
    while (RE2::FindAndConsume(&remaining, token, &window[next])) {
        auto& word = window[next];
        word = lower(std::move(word));
        if (RE2::FullMatch(word, significant))
            result.significant_tokens.push_back(word);
        ++result.words;
        next = (next + 1) % window.size();
        if (result.words >= window.size()) {
            // Only the latest five words contribute to this shingle. Keep the
            // original byte order and separators without retaining prior words.
            std::uint64_t hash = 14695981039346656037ULL;
            for (std::size_t j = 0; j < window.size(); ++j) {
                for (const unsigned char c : window[(next + j) % window.size()])
                    hash = (hash ^ c) * 1099511628211ULL;
                hash = (hash ^ 0xffU) * 1099511628211ULL;
            }
            result.shingles.push_back(hash);
        }
    }
    // This is a bounded text-similarity sketch, never a cryptographic identity or
    // an assertion of factual agreement. Every excluded document is retained.
    std::sort(result.shingles.begin(), result.shingles.end());
    result.shingles.erase(std::unique(result.shingles.begin(), result.shingles.end()), result.shingles.end());
    if (result.shingles.size() > 2048) {
        result.shingles.resize(2048);
        result.sampled = true;
    }
    return result;
}
double similarity(const TextSignature& left, const TextSignature& right) {
    if (left.words < 50 || right.words < 50 || left.significant_tokens != right.significant_tokens ||
        static_cast<double>(std::min(left.words, right.words)) /
                static_cast<double>(std::max(left.words, right.words)) <
            0.95)
        return 0;
    std::size_t common = 0, a = 0, b = 0;
    while (a < left.shingles.size() && b < right.shingles.size()) {
        if (left.shingles[a] == right.shingles[b]) {
            ++common;
            ++a;
            ++b;
        } else if (left.shingles[a] < right.shingles[b])
            ++a;
        else
            ++b;
    }
    const auto total = left.shingles.size() + right.shingles.size() - common;
    return total ? static_cast<double>(common) / static_cast<double>(total) : 0;
}
} // namespace devbox::web::detail
