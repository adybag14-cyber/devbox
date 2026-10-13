#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace devbox {
// The buffers are disjoint, tightly packed, top-down rows. Sample the same
// nearest-neighbour pixels as full BGRA->RGB conversion followed by resizing,
// without constructing RGB values for source pixels that will be discarded.
template <class Check>
void sample_bgra_to_rgb(std::span<const std::uint8_t> bgra, unsigned source_width, unsigned source_height,
                        std::span<std::uint8_t> rgb, unsigned width, unsigned height, Check check) {
    const auto source_pixels = static_cast<std::uint64_t>(source_width) * source_height;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || width > source_width || height > source_height ||
        source_pixels > std::numeric_limits<std::size_t>::max() / 4 ||
        pixels > std::numeric_limits<std::size_t>::max() / 3 || bgra.size() != source_pixels * 4 ||
        rgb.size() != pixels * 3)
        throw std::invalid_argument("Invalid BGRA/RGB sampling dimensions or buffer sizes");
    check();
    if (width == source_width && height == source_height) {
        // Preserve the linear conversion path for captures that need no resize.
        for (std::size_t i = 0; i < static_cast<std::size_t>(pixels); ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        return;
    }
    for (unsigned y = 0; y < height; ++y) {
        check();
        const auto source_y = static_cast<std::uint64_t>(y) * source_height / height;
        for (unsigned x = 0; x < width; ++x) {
            const auto source_x = static_cast<std::uint64_t>(x) * source_width / width;
            const auto source = static_cast<std::size_t>(source_y * source_width + source_x) * 4;
            const auto target = (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[target] = bgra[source + 2];
            rgb[target + 1] = bgra[source + 1];
            rgb[target + 2] = bgra[source];
        }
    }
}
} // namespace devbox
