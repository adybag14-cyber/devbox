#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "UTF-8 excerpt selection and offset reporting"}}
                         .dump()
                  << '\n';
        for (const bool unicode : {false, true}) {
            for (const std::size_t bytes : {4096U, 65536U}) {
                std::string text;
                const std::string segment =
                    unicode ? "Price evidence \xce\xb1\xe4\xb8\xad\xf0\x9f\x93\xb1 details. "
                            : "Price evidence and specification details. ";
                while (text.size() < bytes)
                    text += segment;
                text += " TARGET_MATCH_TOKEN final source context.";
                for (const std::string query : {"", "TARGET_MATCH_TOKEN", "NoSuchTerm"}) {
                    const auto expected = web::evidence_excerpt_details(text, query, 350).dump();
                    for (unsigned trial = 0; trial < 5; ++trial) {
                        std::uint64_t calls = 0, allocated = 0;
                        double milliseconds = 0;
                        for (unsigned i = 0; i < 100; ++i) {
                            const auto before = allocator_counters();
                            const auto began = Clock::now();
                            auto result = web::evidence_excerpt_details(text, query, 350);
                            milliseconds +=
                                std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                            const auto after = allocator_counters();
                            calls += after.calls - before.calls;
                            allocated += after.allocated - before.allocated;
                            if (result.dump() != expected)
                                throw Error("Excerpt content or offsets changed during measurement");
                        }
                        std::cout << Json{{"unicode", unicode},
                                          {"input_bytes", text.size()},
                                          {"query", query},
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
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
