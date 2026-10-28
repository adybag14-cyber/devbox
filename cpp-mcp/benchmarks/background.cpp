#include "devbox/background.hpp"
#include "devbox/allocator.hpp"
#include <iostream>

using namespace devbox;
namespace {
template <class Fn>
void measure(std::string_view phase, std::size_t workers, std::size_t count, unsigned trial, Fn operation) {
    std::uint64_t observed = 0;
    for (unsigned i = 0; i < 3; ++i)
        observed += operation(i);
    const auto before = allocator_counters();
    const auto started = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
        observed += operation(i);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto after = allocator_counters();
    std::cout << Json{{"phase", phase},
                      {"workers", workers},
                      {"iterations", count},
                      {"trial", trial},
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
        for (const std::size_t workers : {1, 14, 128}) {
            BackgroundTasks background;
            std::vector<std::string> names;
            for (std::size_t i = 0; i < workers; ++i) {
                names.push_back("fixture-" + std::to_string(i));
                background.mark_started(names.back(), i % 2 ? "periodic" : "event-driven");
                background.success(names.back());
            }
            const std::string error = "A bounded diagnostic failure used to measure native state updates";
            for (unsigned trial = 0; trial < 5; ++trial) {
                measure("attempt_success", workers, 20000, trial, [&](std::size_t i) {
                    const auto& name = names[i % workers];
                    background.attempt(name);
                    background.success(name);
                    return 1U;
                });
                measure("failure_recovery", workers, 2000, trial, [&](std::size_t i) {
                    const auto& name = names[i % workers];
                    background.failure(name, error);
                    background.success(name);
                    return 1U;
                });
                measure("snapshot", workers,
                        workers == 1    ? 10000
                        : workers == 14 ? 2000
                                        : 250,
                        trial, [&](std::size_t) { return background.snapshot().size(); });
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
