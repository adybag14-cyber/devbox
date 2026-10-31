#pragma once
#include <string_view>

namespace devbox {
// RFC 3629 section 4. Validate the whole input without allocating or replacing
// bytes: overlong forms, surrogate code points and values above U+10FFFF fail.
#if defined(__GNUC__) && defined(__x86_64__)
// Keep the small ASCII loop within an instruction-cache line when surrounding
// extraction code changes layout. The scalar validation rules are unchanged.
[[gnu::aligned(64)]]
#endif
constexpr bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        const std::size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                                   : lead >= 0xe0 && lead <= 0xef ? 3
                                   : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                  : 0;
        if (!length || text.size() - i < length)
            return false;
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90))
            return false;
        for (std::size_t j = 1; j < length; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80)
                return false;
        i += length;
    }
    return true;
}
} // namespace devbox
