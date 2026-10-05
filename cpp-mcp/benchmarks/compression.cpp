#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <libdeflate.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
namespace {
std::string buffered(std::string_view input) {
    std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)> codec(
        libdeflate_alloc_compressor(1), libdeflate_free_compressor);
    if (!codec)
        throw std::runtime_error("libdeflate allocation failed");
    std::array<char, 4096> probe{};
    if (!libdeflate_gzip_compress(codec.get(), input.data(), 8192, probe.data(), probe.size()))
        return {};
    std::string output(input.size() / 2, '\0');
    const auto size =
        libdeflate_gzip_compress(codec.get(), input.data(), input.size(), output.data(), output.size());
    output.resize(size);
    return output;
}
std::string streaming(std::string_view input) {
    z_stream state{};
    if (deflateInit2(&state, Z_BEST_SPEED, Z_DEFLATED, MAX_WBITS + 16, 5, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("zlib initialization failed");
    struct End {
        z_stream* state;
        ~End() {
            deflateEnd(state);
        }
    } end{&state};
    auto block = std::make_unique<std::array<char, 65536>>();
    std::string output;
    output.reserve(block->size());
    for (std::size_t offset = 0; offset < input.size();) {
        const auto count = std::min<std::size_t>(offset ? 65536 : 8192, input.size() - offset);
        state.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + offset));
        state.avail_in = static_cast<uInt>(count);
        offset += count;
        int status;
        do {
            state.next_out = reinterpret_cast<Bytef*>(block->data());
            state.avail_out = static_cast<uInt>(block->size());
            status = deflate(&state, offset == input.size() ? Z_FINISH
                                     : offset == 8192       ? Z_SYNC_FLUSH
                                                            : Z_NO_FLUSH);
            if ((status != Z_OK && status != Z_STREAM_END) || state.total_out >= input.size() / 2)
                return {};
            output.append(block->data(), block->size() - state.avail_out);
        } while (state.avail_in || (offset == input.size() && status != Z_STREAM_END));
        if (output.size() > offset / 2)
            return {};
    }
    return output;
}
void verify(std::string_view compressed, std::string_view expected) {
    if (compressed.empty())
        return;
    z_stream state{};
    if (inflateInit2(&state, MAX_WBITS + 16) != Z_OK)
        throw std::runtime_error("inflate initialization failed");
    struct End {
        z_stream* state;
        ~End() {
            inflateEnd(state);
        }
    } end{&state};
    std::string output(expected.size(), '\0');
    state.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    state.avail_in = static_cast<uInt>(compressed.size());
    state.next_out = reinterpret_cast<Bytef*>(output.data());
    state.avail_out = static_cast<uInt>(output.size());
    if (inflate(&state, Z_FINISH) != Z_STREAM_END || state.total_out != expected.size() || output != expected)
        throw std::runtime_error("compression round trip failed");
}
std::string payload(std::size_t bytes, bool unicode, bool noise) {
    std::string text;
    std::uint32_t random = 0x98765432U; // Deterministic benchmark data, never security randomness.
    for (std::size_t i = 0; text.size() + 256 < bytes; ++i) {
        if (noise) {
            static constexpr char alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (unsigned n = 0; n < 128; ++n) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                text += alphabet[random & 63U];
            }
        } else {
            text += unicode ? "Наблюдения прибора сохранены. Измерения и проверка источника. "
                            : "Observed instrument measurements and source verification were recorded. ";
            text += std::to_string(i) + " ";
        }
    }
    Json data{{"jsonrpc", "2.0"}, {"result", Json{{"source_text", text}}}};
    const auto size = data.dump().size();
    if (size > bytes)
        throw std::runtime_error("benchmark payload exceeded requested size");
    data["result"]["source_text"] = text + std::string(bytes - size, ' ');
    return data.dump();
}
Json summarize(std::vector<double> samples, std::size_t output_bytes) {
    std::sort(samples.begin(), samples.end());
    return Json{{"samples", samples.size()},
                {"p50_ms", samples[samples.size() / 2]},
                {"p95_ms", samples[std::min(samples.size() - 1, samples.size() * 95 / 100)]},
                {"compressed_bytes", output_bytes},
                {"accepted", output_bytes != 0}};
}
} // namespace
int main(int argc, char** argv) {
    try {
        const bool verification = argc == 2 && std::string_view(argv[1]) == "--verify";
        if (argc > 1 && !verification)
            throw std::runtime_error("Usage: devbox-compression-bench [--verify]");
        Json cases = Json::array();
        for (const auto& name : {"ascii_128k", "unicode_512k", "ascii_1m", "noise_512k"}) {
            const std::string label(name);
            const auto bytes = label == "ascii_128k" ? 128U * 1024U
                               : label == "ascii_1m" ? 1024U * 1024U
                                                     : 512U * 1024U;
            const bool noise = label.starts_with("noise");
            const auto input = payload(bytes, label.starts_with("unicode"), noise);
            std::array<std::vector<double>, 2> samples;
            std::array<std::size_t, 2> sizes{};
            const unsigned warmup = verification ? 0 : 3, count = verification ? 1 : 25;
            for (unsigned n = 0; n < warmup + count; ++n)
                for (unsigned order = 0; order < 2; ++order) {
                    const auto choice = (order + n) % 2;
                    const auto start = Clock::now();
                    const auto output = choice ? streaming(input) : buffered(input);
                    const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                    if (output.empty() != noise)
                        throw std::runtime_error("compression acceptance changed for " + label);
                    verify(output, input);
                    sizes[choice] = output.size();
                    if (n >= warmup)
                        samples[choice].push_back(ms);
                }
            cases.push_back(Json{{"case", label},
                                 {"input_bytes", input.size()},
                                 {"input_crc32", crc32(0, reinterpret_cast<const Bytef*>(input.data()),
                                                       static_cast<uInt>(input.size()))},
                                 {"libdeflate_buffered", summarize(std::move(samples[0]), sizes[0])},
                                 {"zlib_streaming", summarize(std::move(samples[1]), sizes[1])},
                                 {"roundtrip_verified", !noise}});
        }
        std::cout << Json{{"ok", true},
                          {"method", "Paired component samples, alternating order; allocation and 50-percent "
                                     "acceptance policy included. No end-to-end latency claim."},
                          {"cases", cases}}
                         .dump(2)
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
