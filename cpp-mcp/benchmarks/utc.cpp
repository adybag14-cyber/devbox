#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout
            << Json{{"build", build_snapshot()}, {"scope", "Native UTC formatting and byte equality"}}.dump()
            << '\n';
        for (const auto base : {0LL, 951782399000LL, 1793563200000LL}) {
            std::vector<std::int64_t> inputs;
            std::vector<std::string> expected;
            std::string combined;
            for (unsigned i = 0; i < 10000; ++i) {
                inputs.push_back(base + static_cast<std::int64_t>(i) * 883631);
                expected.push_back(utc_from_millis(inputs.back()));
                combined += expected.back();
            }
            for (unsigned trial = 0; trial < 5; ++trial) {
                const auto before = allocator_counters();
                const auto began = Clock::now();
                for (std::size_t i = 0; i < inputs.size(); ++i)
                    if (utc_from_millis(inputs[i]) != expected[i])
                        throw Error("UTC bytes changed during measurement");
                const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                const auto after = allocator_counters();
                std::cout << Json{{"base_milliseconds", base},
                                  {"trial", trial},
                                  {"iterations", inputs.size()},
                                  {"milliseconds", elapsed},
                                  {"allocation_calls", after.calls - before.calls},
                                  {"allocated_bytes", after.allocated - before.allocated},
                                  {"retained_bytes_delta", static_cast<std::int64_t>(after.current) -
                                                               static_cast<std::int64_t>(before.current)},
                                  {"output_sha256", sha256(combined)}}
                                 .dump()
                          << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
