#include "../src/research_signature.hpp"
#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include <iostream>
using namespace devbox;
Json encoded(const web::detail::TextSignature& signature) {
    return Json{{"words", signature.words},
                {"sampled", signature.sampled},
                {"shingles", signature.shingles},
                {"significant_tokens", signature.significant_tokens}};
}
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()}, {"scope", "Research near-duplicate signatures"}}.dump()
                  << '\n';
        for (const bool long_words : {false, true}) {
            for (const std::size_t bytes : {4096U, 65536U}) {
                std::string text;
                unsigned row = 0;
                while (text.size() < bytes) {
                    text += long_words ? "PhotodetectorCalibration ThermodynamicObservations "
                                         "SpectrophotometerUncertainty "
                                       : "The measured signal has independent evidence ";
                    text += "record" + std::to_string(row++) + " without changes \xce\xb1\xe4\xb8\xad. ";
                }
                const auto expected = encoded(web::detail::content_signature(text)).dump();
                for (unsigned trial = 0; trial < 5; ++trial) {
                    std::uint64_t calls = 0, allocated = 0;
                    double milliseconds = 0;
                    for (unsigned iteration = 0; iteration < 100; ++iteration) {
                        const auto before = allocator_counters();
                        const auto began = Clock::now();
                        auto result = web::detail::content_signature(text);
                        milliseconds +=
                            std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                        const auto after = allocator_counters();
                        calls += after.calls - before.calls;
                        allocated += after.allocated - before.allocated;
                        if (encoded(result).dump() != expected)
                            throw Error("Research signature changed during measurement");
                    }
                    std::cout << Json{{"long_words", long_words},
                                      {"input_bytes", text.size()},
                                      {"trial", trial},
                                      {"iterations", 100},
                                      {"milliseconds", milliseconds},
                                      {"allocation_calls", calls},
                                      {"allocated_bytes", allocated},
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
