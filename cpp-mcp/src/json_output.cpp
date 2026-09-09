#include "devbox/common.hpp"
#include <algorithm>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define DEVBOX_JSON_SSE2 1
#endif

namespace devbox {
namespace {
bool plain_ascii(std::string_view text) {
#ifdef DEVBOX_JSON_SSE2
    // SSE2 is a baseline instruction set on these targets. Signed comparison
    // also rejects bytes with the high bit set, so UTF-8 retains the reference
    // serializer. Only load complete blocks; the remainder is checked below.
    const auto space = _mm_set1_epi8(0x20);
    const auto quote = _mm_set1_epi8('"');
    const auto slash = _mm_set1_epi8('\\');
    while (text.size() >= 16) {
        const auto bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(text.data()));
        const auto invalid = _mm_or_si128(_mm_cmplt_epi8(bytes, space),
                                         _mm_or_si128(_mm_cmpeq_epi8(bytes, quote),
                                                      _mm_cmpeq_epi8(bytes, slash)));
        if (_mm_movemask_epi8(invalid))
            return false;
        text.remove_prefix(16);
    }
#endif
    // Preserve the scalar path on other targets and for a partial final block.
    unsigned invalid = 0;
    for (unsigned char c : text)
        invalid |= static_cast<unsigned>(c < 0x20) | static_cast<unsigned>(c >= 0x80) |
                   static_cast<unsigned>(c == '"') | static_cast<unsigned>(c == '\\');
    return !invalid;
}
void append_string(std::string& output, const std::string& text, Json::error_handler_t errors) {
    // Printable ASCII needs neither escaping nor UTF-8 repair. Larger strings
    // can be copied directly after validation; all other encodings retain the
    // reference serializer's exact escaping and error policy.
    if (plain_ascii(text)) {
        if (text.size() > 1024 && text.size() > output.capacity() - output.size()) {
            const auto available = output.max_size() - output.size();
            if (text.size() > available - std::min<std::size_t>(available, 2))
                throw std::length_error("JSON string exceeds output size limit");
            // Leave room for quotes, delimiters and nearby metadata. Growing
            // exactly to the payload would copy it again for the closing quote.
            const auto extra = std::min<std::size_t>(1024, available - text.size());
            output.reserve(output.size() + text.size() + extra);
        }
        output += '"';
        output += text;
        output += '"';
    } else
        output += Json(text).dump(-1, ' ', false, errors);
}
void append_value(std::string& output, const Json& value, Json::error_handler_t errors) {
    if (value.is_string())
        append_string(output, value.get_ref<const std::string&>(), errors);
    else if (value.is_array()) {
        output += '[';
        bool first = true;
        for (const auto& item : value) {
            if (!first)
                output += ',';
            first = false;
            append_value(output, item, errors);
        }
        output += ']';
    } else if (value.is_object()) {
        output += '{';
        bool first = true;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!first)
                output += ',';
            first = false;
            append_string(output, it.key(), errors);
            output += ':';
            append_value(output, it.value(), errors);
        }
        output += '}';
    } else
        output += value.dump(-1, ' ', false, errors);
}
} // namespace
std::string json_dump(const Json& value, Json::error_handler_t errors) {
    std::string output;
    output.reserve(1024);
    append_value(output, value, errors);
    return output;
}
} // namespace devbox
