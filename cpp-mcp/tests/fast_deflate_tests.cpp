#include "devbox/fast_deflate.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>
#ifndef _WIN32
#include <pthread.h>
#endif
using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void decode(std::string_view encoded, std::string_view expected) {
    z_stream stream{};
    require(inflateInit2(&stream, MAX_WBITS + 32) == Z_OK, "Independent decoder initialization");
    struct End {
        z_stream& stream;
        ~End() {
            inflateEnd(&stream);
        }
    } end{stream};
    std::string output(expected.size() + 1, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(encoded.data()));
    stream.avail_in = static_cast<uInt>(encoded.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    require(inflate(&stream, Z_FINISH) == Z_STREAM_END,
            "Independent gzip/zlib decoder rejected native stream");
    require(stream.avail_in == 0 && stream.total_out == expected.size(), "Native stream length mismatch");
    output.resize(static_cast<std::size_t>(stream.total_out));
    require(output == expected, "Native compression changed payload bytes");
}
std::string bytes(std::size_t count, unsigned kind, std::uint32_t seed) {
    std::string out(count, '\0');
    constexpr std::string_view pattern = "{\"product\":\"phone\",\"price\":899,\"currency\":\"GBP\"}\n";
    for (std::size_t i = 0; i < count; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        out[i] = kind == 0   ? static_cast<char>(seed & 255)
                 : kind == 1 ? pattern[i % pattern.size()]
                 : kind == 2 ? static_cast<char>(i & 255)
                             : 'x';
    }
    return out;
}
void round_trip(std::string_view input, DeflateWrapper wrapper) {
    std::vector<char> output(input.size() + input.size() / 8 + 66, '\x5a');
    const auto count = native_deflate(input, std::span(output).subspan(1, output.size() - 2), wrapper);
    require(count != 0, "Worst-case bounded output did not fit");
    require(output.front() == '\x5a' && output.back() == '\x5a', "Native encoder overwrote output bounds");
    decode({output.data() + 1, count}, input);
    // Exact fit and every smaller output bound exercise partial-output refusal.
    const auto limits = input.size() < 100 ? count : std::min<std::size_t>(count, 32);
    for (std::size_t limit = 0; limit < limits; ++limit) {
        std::vector<char> short_output(limit + 2, '\x5a');
        require(native_deflate(input, std::span(short_output).subspan(1, limit), wrapper) == 0,
                "An undersized destination was accepted");
        require(short_output.front() == '\x5a' && short_output.back() == '\x5a',
                "Short-output guard overwritten");
    }
    std::vector<char> exact(count);
    require(native_deflate(input, exact, wrapper) == count, "Exact output capacity failed");
    decode({exact.data(), exact.size()}, input);
}
} // namespace
int main() {
    try {
#ifndef _WIN32
        // Match musl's small worker stack, even when the test host normally
        // grants multi-megabyte stacks. The old 192 KiB automatic scratch
        // crashed the real transport test on Alpine.
        std::exception_ptr stack_error;
        pthread_attr_t attributes;
        require(pthread_attr_init(&attributes) == 0, "Small-stack attributes");
        const auto configured = pthread_attr_setstacksize(&attributes, 128 * 1024);
        if (configured) {
            pthread_attr_destroy(&attributes);
            throw std::runtime_error("Small-stack size unavailable");
        }
        pthread_t worker{};
        const auto created = pthread_create(
            &worker, &attributes,
            [](void* argument) -> void* {
                try {
                    for (const auto wrapper : {DeflateWrapper::gzip, DeflateWrapper::zlib})
                        round_trip(bytes(1024 * 1024, 1, 123), wrapper);
                } catch (...) {
                    *static_cast<std::exception_ptr*>(argument) = std::current_exception();
                }
                return nullptr;
            },
            &stack_error);
        pthread_attr_destroy(&attributes);
        require(created == 0, "Small-stack worker creation");
        require(pthread_join(worker, nullptr) == 0, "Small-stack worker join");
        if (stack_error)
            std::rethrow_exception(stack_error);
#endif
        for (const auto wrapper : {DeflateWrapper::gzip, DeflateWrapper::zlib}) {
            for (const auto size :
                 {0u,   1u,   2u,   3u,    4u,    5u,    7u,     8u,     127u,   255u,    256u,
                  257u, 258u, 259u, 8191u, 8192u, 8193u, 32767u, 32768u, 32769u, 1048576u})
                for (unsigned kind = 0; kind < 4; ++kind)
                    round_trip(bytes(size, kind, 0x13579bdu), wrapper);
            for (unsigned trial = 1; trial <= 100; ++trial)
                round_trip(bytes((trial * 3931u) % 65536u, trial % 4, trial), wrapper);
            auto distant = bytes(32768, 0, 1234567);
            distant += distant;
            round_trip(distant, wrapper);
            // Skewed alphabets exercise dynamic code lengths and their fixed-tree
            // fallback; changing the prefix crosses byte and block boundaries.
            for (unsigned trial = 1; trial <= 64; ++trial) {
                auto skewed = bytes(32700u + trial * 3u, 0, trial);
                for (std::size_t i = 0; i < skewed.size(); ++i) {
                    const auto value = static_cast<unsigned char>(skewed[i]);
                    skewed[i] = static_cast<char>(value < 240 ? value % (trial % 7 + 1) : value);
                }
                round_trip(skewed, wrapper);
            }
        }
        struct Interrupted {};
        unsigned checks = 0;
        const auto input = bytes(65536, 1, 1);
        std::vector<char> output(input.size());
        bool cancelled = false;
        try {
            (void)native_deflate(input, output, DeflateWrapper::gzip, [&] {
                if (++checks == 3)
                    throw Interrupted{};
            });
        } catch (const Interrupted&) {
            cancelled = true;
        }
        require(cancelled && checks == 3, "Cancellation was not propagated between bounded work slices");
        bool oversized = false;
        try {
            (void)native_deflate(std::string(1048577, 'x'), output, DeflateWrapper::gzip);
        } catch (const std::invalid_argument&) {
            oversized = true;
        }
        require(oversized, "Native whole-buffer input limit was not enforced");
        std::cout << "native DEFLATE: independent round trips, limits, windows and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
