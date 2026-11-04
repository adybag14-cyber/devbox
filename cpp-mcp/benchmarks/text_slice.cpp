#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "Bounded UTF16-compatible text slicing; excludes network and model time"}}
                         .dump()
                  << '\n';
        for (const std::size_t size : {131072U, 4194304U}) {
            for (const auto* kind :
                 {"ascii_prefix", "ascii_tail", "unicode_after_prefix", "unicode_prefix"}) {
                std::string input;
                const std::string line =
                    "Checked product record: price GBP 649.00, source and observation timestamp.\n";
                while (input.size() < size)
                    input += line;
                input.resize(size);
                if (std::string_view(kind) == "unicode_after_prefix")
                    input += "\xc2\xa3\xf0\x9f\x93\xb1";
                if (std::string_view(kind) == "unicode_prefix")
                    input.insert(0, "\xc2\xa3");
                const auto units = to_utf16(input);
                const bool tail = std::string_view(kind) == "ascii_tail";
                const auto start = tail ? units.size() - 200 : 0;
                const auto end = tail ? units.size() : 200;
                const auto expected = from_utf16(std::u16string_view(units).substr(start, end - start));
                const auto iterations = size > 131072 ? 16U : 128U;
                if (js_slice(input, start, end) != expected)
                    throw Error("Slice warm-up differs from reference UTF16 output");
                for (unsigned trial = 0; trial < 5; ++trial) {
                    const auto before = allocator_counters();
                    const auto began = Clock::now();
                    for (unsigned iteration = 0; iteration < iterations; ++iteration)
                        if (js_slice(input, start, end) != expected)
                            throw Error("Slice differs from reference UTF16 output");
                    const auto elapsed =
                        std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                    const auto after = allocator_counters();
                    std::cout << Json{{"kind", kind},
                                      {"input_bytes", input.size()},
                                      {"trial", trial},
                                      {"iterations", iterations},
                                      {"start", start},
                                      {"end", end},
                                      {"milliseconds", elapsed},
                                      {"allocation_calls", after.calls - before.calls},
                                      {"allocated_bytes", after.allocated - before.allocated},
                                      {"retained_bytes_delta", static_cast<std::int64_t>(after.current) -
                                                                   static_cast<std::int64_t>(before.current)},
                                      {"output_sha256", sha256(expected)}}
                                     .dump()
                              << '\n';
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
