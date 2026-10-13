#include "devbox/capture_pixels.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <vector>

void require(bool condition) {
    if (!condition)
        throw std::runtime_error("Capture sampling oracle failed");
}

std::vector<std::uint8_t> reference(const std::vector<std::uint8_t>& bgra, unsigned sw, unsigned sh,
                                    unsigned w, unsigned h) {
    std::vector<std::uint8_t> full(static_cast<std::size_t>(sw) * sh * 3);
    for (std::size_t i = 0; i < full.size() / 3; ++i) {
        full[i * 3] = bgra[i * 4 + 2];
        full[i * 3 + 1] = bgra[i * 4 + 1];
        full[i * 3 + 2] = bgra[i * 4];
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 3);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const auto sy = static_cast<std::uint64_t>(y) * sh / h;
            const auto sx = static_cast<std::uint64_t>(x) * sw / w;
            const auto at = static_cast<std::size_t>(sy * sw + sx) * 3;
            std::copy_n(full.data() + at, 3, out.data() + (static_cast<std::size_t>(y) * w + x) * 3);
        }
    return out;
}

int main() {
    try {
        std::mt19937 random(0x20261013);
        unsigned cases = 0;
        for (const auto shape :
             {std::array{1u, 1u}, std::array{1u, 43u}, std::array{71u, 1u}, std::array{13u, 17u},
              std::array{641u, 479u}, std::array{1920u, 1080u}, std::array{3840u, 2160u}}) {
            const auto sw = shape[0], sh = shape[1];
            std::vector<std::uint8_t> bgra(static_cast<std::size_t>(sw) * sh * 4);
            for (auto& value : bgra)
                value = static_cast<std::uint8_t>(random());
            const auto saved = bgra;
            for (const auto limit : {1u, 2u, 7u, 640u, 1600u, 4096u}) {
                const auto w = std::min(sw, limit);
                const auto h = std::max(1u, static_cast<unsigned>(static_cast<std::uint64_t>(sh) * w / sw));
                const auto expected = reference(bgra, sw, sh, w, h);
                std::vector<std::uint8_t> actual(expected.size(), 0xcd);
                devbox::sample_bgra_to_rgb(bgra, sw, sh, actual, w, h, [] {});
                require(actual == expected && bgra == saved);
                ++cases;
            }
        }
        std::vector<std::uint8_t> source(8 * 8 * 4, 17), out(4 * 4 * 3, 0xcd);
        unsigned calls = 0;
        try {
            devbox::sample_bgra_to_rgb(source, 8, 8, out, 4, 4, [&] {
                if (++calls == 3)
                    throw std::runtime_error("cancelled fixture");
            });
            require(false);
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "cancelled fixture" && calls == 3);
        }
        require(std::all_of(out.begin() + 12, out.end(), [](auto value) { return value == 0xcd; }));
        for (const auto shape : {std::array{0u, 4u}, std::array{4u, 0u}, std::array{9u, 4u},
                                 std::array{4u, 9u}, std::array{3u, 4u}}) {
            out.assign(4 * 4 * 3, 0xcd);
            bool rejected = false;
            try {
                devbox::sample_bgra_to_rgb(source, 8, 8, out, shape[0], shape[1], [] {});
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected &&
                    std::all_of(out.begin(), out.end(), [](auto value) { return value == 0xcd; }));
        }
        bool rejected = false;
        try {
            devbox::sample_bgra_to_rgb({}, UINT32_MAX, UINT32_MAX, {}, 1, 1, [] {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected);
        std::cout << cases << " pixel parity cases, cancellation and bounds checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
