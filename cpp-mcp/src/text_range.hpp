#pragma once
#include "devbox/common.hpp"
#include <algorithm>

namespace devbox::detail {
inline constexpr std::uint64_t text_range_max_offset = 9007199254740991ULL;

inline std::size_t text_range_budget(std::uint64_t offset, std::size_t requested) {
    if (requested < 4 || requested > 65536 || offset > text_range_max_offset)
        throw Error("READ_TEXT_RANGE_LIMIT: use 4-65536 bytes and a safe nonnegative byte offset");
    return static_cast<std::size_t>(std::min<std::uint64_t>(requested, text_range_max_offset - offset));
}

struct TextRangeBoundary {
    std::size_t bytes;
    bool eof;
    bool adjusted;
};

// The file reader supplies at most budget+1 bytes. Validate the complete return
// prefix and every observed byte of a scalar starting in it, including lookahead.
// No replacement encoding, unbounded scan or second text allocation is needed.
inline TextRangeBoundary text_range_boundary(std::string_view observed, std::uint64_t offset,
                                             std::size_t requested) {
    const auto budget = text_range_budget(offset, requested);
    if (observed.size() > budget + 1)
        throw Error("READ_TEXT_RANGE_LIMIT: unexpected lookahead size");
    if (offset && budget && !observed.empty() &&
        (static_cast<unsigned char>(observed.front()) & 0xc0) == 0x80)
        throw Error("READ_TEXT_RANGE_OFFSET: start at a UTF-8 character boundary; use next_offset_bytes");
    const auto end = std::min(observed.size(), budget);
    const bool more = observed.size() > budget;
    std::size_t cursor = 0;
    while (cursor < end) {
        const auto lead = static_cast<unsigned char>(observed[cursor]);
        const std::size_t width = lead < 0x80                    ? 1
                                  : lead >= 0xc2 && lead <= 0xdf ? 2
                                  : lead >= 0xe0 && lead <= 0xef ? 3
                                  : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                 : 0;
        if (!width)
            throw Error("READ_TEXT_RANGE_UTF8: malformed UTF-8 leading byte");
        const auto available = std::min(width, observed.size() - cursor);
        for (std::size_t i = 1; i < available; ++i) {
            const auto byte = static_cast<unsigned char>(observed[cursor + i]);
            if ((byte & 0xc0) != 0x80 ||
                (i == 1 && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte > 0x9f) ||
                            (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte > 0x8f))))
                throw Error("READ_TEXT_RANGE_UTF8: malformed UTF-8 continuation");
        }
        if (available < width && !more)
            throw Error("READ_TEXT_RANGE_UTF8: incomplete UTF-8 at EOF");
        if (width > end - cursor)
            break;
        cursor += width;
    }
    if (!cursor && !observed.empty())
        throw Error("READ_TEXT_RANGE_LIMIT: no complete scalar fits before the safe offset limit");
    return {cursor, !more, cursor < end};
}
} // namespace devbox::detail
