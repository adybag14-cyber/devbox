#pragma once
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

namespace devbox {
enum class DeflateWrapper { gzip, zlib };
// RFC 1951 Huffman/LZ77 encoder for bounded responses, with no retained
// dictionary and no dynamic scratch allocation. Zero means the destination did
// not fit. Cancellation is checked at most 9 KiB of input apart. The caller must
// discard the destination on zero or exception. Larger responses use streaming zlib.
std::size_t native_deflate(std::string_view input, std::span<char> destination, DeflateWrapper wrapper,
                           const std::function<void()>& check = {});
} // namespace devbox
