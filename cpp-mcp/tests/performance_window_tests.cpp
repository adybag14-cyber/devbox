#include "devbox/performance_window.hpp"
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>

using devbox::detail::PerformanceSample;
using devbox::detail::PerformanceWindow;
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void compare(std::size_t capacity, std::int64_t cadence) {
    PerformanceWindow window(capacity);
    std::deque<PerformanceSample> reference;
    std::vector<PerformanceSample> snapshot;
    auto now = Clock::time_point{};
    for (std::int64_t i = 0; i < 80000; ++i) {
        // Fixed deterministic traces include ordinary cadence, a timer backlog
        // with equal timestamps, multiple wraps, and expiry after suspension.
        now += Millis(i % 27011 == 0 ? 300001 : (cadence == 20 && i % 101 < 40 ? 0 : cadence));
        const PerformanceSample sample{now, static_cast<double>((i * 7919) % 997) / 16.0};
        window.push(sample);
        reference.push_back(sample);
        while (reference.size() > capacity ||
               (!reference.empty() && now - reference.front().at > std::chrono::minutes(5)))
            reference.pop_front();
        if (i % 211 == 0 || i == 79999) {
            window.copy_to(snapshot);
            require(std::equal(snapshot.begin(), snapshot.end(), reference.begin(), reference.end()),
                    "bounded history differs from chronological reference");
        }
    }
}
} // namespace

int main() {
    try {
        bool rejected = false;
        try {
            PerformanceWindow invalid(0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "zero capacity is rejected");
        PerformanceWindow window(3);
        std::vector<PerformanceSample> values{{Clock::time_point{}, -1}};
        window.copy_to(values);
        require(values.empty(), "an empty history clears the output");
        const auto start = Clock::time_point{};
        for (int i = 0; i < 4; ++i)
            window.push({start, static_cast<double>(i)});
        window.copy_to(values);
        require(values.size() == 3 && values.front().ms == 1 && values.back().ms == 3,
                "capacity retains the newest samples in chronological order");
        window.push({start + std::chrono::minutes(5), 4});
        window.copy_to(values);
        require(values.size() == 3 && values.front().ms == 2,
                "inclusive five-minute boundary preserves the oldest retained sample");
        window.push({start + std::chrono::minutes(5) + Millis(1), 5});
        window.copy_to(values);
        require(values.size() == 2 && values.front().ms == 4 && values.back().ms == 5,
                "expired samples leave no stale entries after wraparound");
        compare(1, 20);
        compare(15000, 20);
        compare(301, 1000);
        std::cout << "Bounded telemetry history, wraparound, expiry and reference equivalence passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
