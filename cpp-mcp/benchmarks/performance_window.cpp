#include "devbox/performance_window.hpp"
#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include <deque>
#include <iostream>

using namespace devbox;
using Sample = detail::PerformanceSample;
namespace {
// The previous implementation is retained here only as a measured reference.
class DequeWindow {
    std::deque<Sample> data_;
    std::size_t capacity_;

  public:
    explicit DequeWindow(std::size_t capacity) : capacity_(capacity) {}
    void push(Sample value) {
        data_.push_back(value);
        while (data_.size() > capacity_ ||
               (!data_.empty() && value.at - data_.front().at > std::chrono::minutes(5)))
            data_.pop_front();
    }
    void copy_to(std::vector<Sample>& out) const {
        out.assign(data_.begin(), data_.end());
    }
};

template <class Window> std::uint64_t measure(std::string_view name, int round, bool capture) {
    const auto before = allocator_counters();
    const auto started = Clock::now();
    Window delays(15000), drifts(301);
    std::uint64_t checksum = 0;
    for (std::int64_t i = 0; i < 180000; ++i) {
        const auto at = Clock::time_point{} + Millis(i * 20);
        const auto value = static_cast<double>((i * 7919) % 997) / 16.0;
        delays.push({at, value});
        if (i % 50 == 0)
            drifts.push({at, value});
        if (capture && i % 250 == 0) {
            std::vector<Sample> d, r;
            delays.copy_to(d);
            drifts.copy_to(r);
            for (const auto seconds : {10, 60, 300}) {
                const auto cutoff = at - std::chrono::seconds(seconds);
                std::vector<double> values;
                double drift = 0;
                for (const auto& sample : d)
                    if (sample.at >= cutoff)
                        values.push_back(sample.ms);
                for (const auto& sample : r)
                    if (sample.at >= cutoff)
                        drift = std::max(drift, sample.ms);
                std::sort(values.begin(), values.end());
                for (const std::size_t p : {50, 95, 99})
                    checksum = checksum * 31 +
                               static_cast<std::uint64_t>(values[((values.size() - 1) * p + 50) / 100] * 16);
                checksum = checksum * 31 + values.size() + static_cast<std::uint64_t>(drift * 16);
            }
        }
    }
    {
        std::vector<Sample> d, r;
        delays.copy_to(d);
        drifts.copy_to(r);
        checksum += d.size() + r.size() + static_cast<std::uint64_t>(d.back().ms * 16);
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto after = allocator_counters();
    std::cout << Json{{"variant", name},
                      {"round", round},
                      {"capture", capture},
                      {"simulated_ticks", 180000},
                      {"elapsed_ms", elapsed},
                      {"allocation_calls", after.calls - before.calls},
                      {"allocated_bytes", after.allocated - before.allocated},
                      {"retained_history_bytes", after.current - before.current},
                      {"checksum", std::to_string(checksum)}}
                     .dump()
              << '\n'
              << std::flush;
    return checksum;
}
} // namespace

int main() {
    try {
        std::cout << Json{{"benchmark", "performance-window"},
                          {"build", build_snapshot()},
                          {"method", "Three ABBA cycles; accelerated 50 Hz trace with a snapshot every five "
                                     "simulated seconds. Actual native allocator counters and production "
                                     "window class. No real-time or whole-server speed claim."}}
                         .dump()
                  << '\n';
        for (const auto capture : {false, true}) {
            std::optional<std::uint64_t> expected;
            for (int round = 0; round < 3; ++round) {
                for (const auto value : {measure<DequeWindow>("deque", round, capture),
                                         measure<detail::PerformanceWindow>("ring", round, capture),
                                         measure<detail::PerformanceWindow>("ring", round, capture),
                                         measure<DequeWindow>("deque", round, capture)}) {
                    if (expected && *expected != value)
                        throw Error("Telemetry benchmark checksum mismatch");
                    expected = value;
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
