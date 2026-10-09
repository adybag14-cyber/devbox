#include "devbox/allocator.hpp"
#include "devbox/process.hpp"
#include <algorithm>
#include <iostream>

using namespace devbox;
namespace {
bool leading(char byte) {
    return (static_cast<unsigned char>(byte) & 0xc0) != 0x80;
}
CaptureResult oracle(std::string_view input, std::optional<std::size_t> limit) {
    const auto text = sanitize_utf8(input);
    const auto count = static_cast<std::size_t>(std::count_if(text.begin(), text.end(), leading));
    if (!limit || count <= *limit)
        return {text, count, false};
    if (*limit == 0)
        return {"", count, count != 0};
    const auto head_count = *limit / 2, tail_count = *limit - head_count;
    std::size_t head_end = 0, seen = 0;
    while (head_end < text.size()) {
        if (leading(text[head_end])) {
            if (seen == head_count)
                break;
            ++seen;
        }
        ++head_end;
    }
    std::size_t tail_start = text.size();
    seen = 0;
    while (tail_start && seen < tail_count) {
        --tail_start;
        if (leading(text[tail_start]))
            ++seen;
    }
    return {text.substr(0, head_end) + "\n... middle capture omitted " + std::to_string(count - *limit) +
                " characters ...\n" + text.substr(tail_start),
            count, true};
}
void measure(std::string_view name, const std::string& input, std::optional<std::size_t> limit,
             std::size_t chunk, unsigned samples) {
    const auto expected = oracle(input, limit);
    std::vector<double> times;
    times.reserve(samples);
    std::uint64_t allocations = 0, bytes = 0;
    for (unsigned i = 0; i < samples + 3; ++i) {
        const auto before = allocator_counters();
        const auto started = Clock::now();
        CaptureAccumulator capture(limit);
        for (std::size_t offset = 0; offset < input.size(); offset += chunk)
            capture.push(std::string_view(input).substr(offset, chunk));
        capture.finish();
        const auto result = capture.snapshot();
        const auto stopped = Clock::now();
        const auto after = allocator_counters();
        if (result.text != expected.text || result.original_chars != expected.original_chars ||
            result.truncated != expected.truncated)
            throw Error("Capture benchmark disagrees with full-stream Unicode/truncation oracle");
        if (i >= 3) {
            times.push_back(std::chrono::duration<double, std::milli>(stopped - started).count());
            allocations += after.calls - before.calls;
            bytes += after.allocated - before.allocated;
        }
    }
    std::sort(times.begin(), times.end());
    std::cout << Json{{"case", name},
                      {"input_bytes", input.size()},
                      {"chunk_bytes", chunk},
                      {"limit_chars", limit ? Json(*limit) : Json()},
                      {"samples", samples},
                      {"warmup", 3},
                      {"p50_ms", times[(times.size() - 1) / 2]},
                      {"p95_ms", times[(times.size() - 1) * 95 / 100]},
                      {"allocated_bytes_per_call", static_cast<double>(bytes) / samples},
                      {"allocations_per_call", static_cast<double>(allocations) / samples},
                      {"original_scalars", expected.original_chars},
                      {"retained_bytes", expected.text.size()},
                      {"exact_oracle", true}}
                     .dump()
              << '\n'
              << std::flush;
}
std::string repeated(std::string_view pattern, std::size_t bytes) {
    std::string result;
    result.reserve(bytes + pattern.size());
    while (result.size() < bytes)
        result += pattern;
    result.resize(bytes);
    return result;
}
} // namespace
int main() {
    try {
        const auto ascii = repeated("compiler output: sample diagnostic line\n", 1024 * 1024);
        const auto unicode = repeated("compiler é Кириллица 😀\n", 1024 * 1024);
        const auto invalid = repeated("A\xff\xf0\x9f"
                                      "B\xe2\x82",
                                      256 * 1024);
        measure("ascii-zero-1m", ascii, 0, 16384, 20);
        measure("ascii-tail4k-1m", ascii, 4000, 16384, 20);
        measure("ascii-tail64k-1m", ascii, 65536, 16384, 20);
        measure("ascii-unlimited-1m", ascii, {}, 16384, 10);
        measure("unicode-tail4k-1m", unicode, 4000, 16384, 20);
        measure("invalid-tail4k-256k", invalid, 4000, 16384, 20);
        measure("unicode-small-chunks", unicode.substr(0, 4096), 7, 1, 30);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
