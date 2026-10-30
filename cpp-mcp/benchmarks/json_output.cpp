#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include <iostream>
#include <limits>

using namespace devbox;
namespace {
void measure(const char* name, unsigned trial, const Json& value) {
    constexpr unsigned iterations = 200;
    const auto expected = value.dump();
    if (json_dump(value) != expected)
        throw Error("JSON output differs from the reference serializer");
    for (unsigned warm = 0; warm < 3; ++warm)
        (void)json_dump(value);
    std::uint64_t observed = 0;
    const auto before = allocator_counters();
    const auto started = Clock::now();
    for (unsigned n = 0; n < iterations; ++n) {
        const auto output = json_dump(value);
        observed += output.size() + static_cast<unsigned char>(output.front()) +
                    static_cast<unsigned char>(output.back());
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto after = allocator_counters();
    std::cout << Json{{"phase", name},
                      {"trial", trial},
                      {"iterations", iterations},
                      {"output_bytes", expected.size()},
                      {"output_sha256", sha256(expected)},
                      {"wall_ms", elapsed},
                      {"allocation_calls", after.calls - before.calls},
                      {"allocated_bytes", after.allocated - before.allocated},
                      {"retained_bytes_delta",
                       static_cast<std::int64_t>(after.current) - static_cast<std::int64_t>(before.current)},
                      {"observed", observed}}
                     .dump()
              << '\n';
}
} // namespace
int main() {
    try {
        Json jobs = Json::array(), controls = Json::array();
        for (std::uint64_t i = 0; i < 64; ++i)
            jobs.push_back(Json{{"id", i},
                                {"queued", i % 3 == 0},
                                {"bytes", i * 100000},
                                {"duration_ms", i * 17},
                                {"exit_code", static_cast<std::int64_t>(i) - 8},
                                {"started_ms", 1793400000000ULL + i},
                                {"result", nullptr}});
        for (std::uint64_t i = 0; i < 45; ++i)
            controls.push_back(
                Json{{"id", std::to_string(i)},
                     {"label", "Phone £1,099.00 \"Black\""},
                     {"disabled", false},
                     {"checked", i % 5 == 0},
                     {"rect", Json{{"x", i * 30}, {"y", i * 10}, {"width", 220}, {"height", 36}}}});
        const Json status{{"running", true},
                          {"active", 7},
                          {"queued", 2},
                          {"jobs", std::move(jobs)},
                          {"lowest", std::numeric_limits<std::int64_t>::min()},
                          {"highest", std::numeric_limits<std::uint64_t>::max()}};
        const Json browser{{"elements", std::move(controls)},
                           {"viewport", Json{{"width", 1600}, {"height", 900}}},
                           {"scroll", Json{{"x", 0}, {"y", 400}}}};
        const Json output{{"stdout", std::string(65536, 'A')}, {"exitCode", 0}, {"truncated", false}};
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "Synthetic control-response serialization; no HTTP client or model time"}}
                         .dump()
                  << '\n';
        for (unsigned trial = 0; trial < 5; ++trial) {
            measure("numeric_status", trial, status);
            measure("browser_controls", trial, browser);
            measure("large_ascii_output", trial, output);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
