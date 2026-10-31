#include "devbox/allocator.hpp"
#include "devbox/chrome_bridge.hpp"
#include "devbox/contract.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "Owned Chrome response parsing and MCP envelope; no browser, process "
                                    "startup or network timing"}}
                         .dump()
                  << '\n';
        for (const std::size_t bytes : {32768U, 524288U, 4194304U}) {
            const std::string encoded(bytes, 'A');
            const auto wire = Json{{"ok", true},
                                   {"data", {{"tab_id", 41}}},
                                   {"image", {{"mime", "image/jpeg"}, {"base64", encoded}}}}
                                  .dump();
            for (unsigned trial = 0; trial < 5; ++trial) {
                std::uint64_t allocations = 0, allocated = 0, output_bytes = 0;
                double milliseconds = 0;
                for (unsigned iteration = 0; iteration < 20; ++iteration) {
                    // Input preparation is outside the measured adapter; the
                    // transport hands over one already-owned response string.
                    std::string input = wire;
                    const auto before = allocator_counters();
                    const auto started = Clock::now();
                    auto response = chrome_bridge_response(std::move(input));
                    milliseconds += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                    const auto after = allocator_counters();
                    allocations += after.calls - before.calls;
                    allocated += after.allocated - before.allocated;
                    const auto& image = response.at("content").at(1).at("data").get_ref<const std::string&>();
                    if (image != encoded || json_bool(response, "isError"))
                        throw Error("Image payload changed");
                    output_bytes += image.size();
                }
                std::cout << Json{{"payload_bytes", bytes},
                                  {"trial", trial},
                                  {"iterations", 20},
                                  {"milliseconds", milliseconds},
                                  {"allocation_calls", allocations},
                                  {"allocated_bytes", allocated},
                                  {"output_bytes", output_bytes},
                                  {"payload_sha256", sha256(encoded)}}
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
