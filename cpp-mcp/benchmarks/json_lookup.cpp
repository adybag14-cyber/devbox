#include "devbox/allocator.hpp"
#include "devbox/common.hpp"
#include <iostream>

using namespace devbox;
namespace {
template <class Fn> void measure(std::string_view name, unsigned trial, Fn operation) {
    constexpr std::size_t count = 200000;
    std::uint64_t observed = 0;
    for (unsigned n = 0; n < 3; ++n)
        observed += operation();
    const auto before = allocator_counters();
    const auto started = Clock::now();
    for (std::size_t n = 0; n < count; ++n)
        observed += operation();
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto after = allocator_counters();
    std::cout << Json{{"phase", name},
                      {"trial", trial},
                      {"iterations", count},
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
        const std::string key(96, 'k'), absent(96, 'z');
        const std::string_view view(key), missing(absent);
        const Json text{{key, "ready"}}, number{{key, 42}}, flag{{key, true}}, fraction{{key, 1.5}},
            empty{{key, nullptr}}, small{{"key", 42}};
        for (unsigned trial = 0; trial < 5; ++trial) {
            measure("long_string", trial, [&] { return json_string(text, view).size(); });
            measure("long_uint", trial, [&] { return json_uint(number, view); });
            measure("long_bool", trial, [&] { return json_bool(flag, view); });
            measure("long_number", trial, [&] { return json_number(fraction, view) == 1.5; });
            measure("long_null", trial, [&] { return json_uint(empty, view, 7); });
            measure("long_missing", trial, [&] { return json_uint(number, missing, 9); });
            measure("short_uint", trial, [&] { return json_uint(small, "key"); });
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
