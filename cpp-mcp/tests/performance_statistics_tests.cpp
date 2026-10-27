#include "devbox/performance_statistics.hpp"
#include <iostream>
#include <random>

using namespace devbox::detail;
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
PerformanceStatistics reference(const std::vector<PerformanceSample>& delays,
                                const std::vector<PerformanceSample>& drifts, Clock::time_point cutoff) {
    PerformanceStatistics result;
    std::vector<double> values;
    for (const auto& sample : delays)
        if (sample.at >= cutoff)
            values.push_back(sample.ms);
    for (const auto& sample : drifts)
        if (sample.at >= cutoff)
            result.drift_maximum = std::max(result.drift_maximum, sample.ms);
    std::sort(values.begin(), values.end());
    result.count = values.size();
    if (!values.empty()) {
        result.p50 = values[((values.size() - 1) * 50 + 50) / 100];
        result.p95 = values[((values.size() - 1) * 95 + 50) / 100];
        result.p99 = values[((values.size() - 1) * 99 + 50) / 100];
        result.maximum = values.back();
    }
    return result;
}
void check(const std::vector<PerformanceSample>& delays, const std::vector<PerformanceSample>& drifts,
           Clock::time_point cutoff, std::vector<double>& scratch) {
    const auto before_delays = delays, before_drifts = drifts;
    require(performance_statistics(delays, drifts, cutoff, scratch) == reference(delays, drifts, cutoff),
            "partial selection differs from the full-sort reference");
    require(delays == before_delays && drifts == before_drifts,
            "capture must not mutate chronological samples");
}
} // namespace

int main() {
    try {
        std::mt19937_64 random(0x41971);
        std::vector<double> scratch;
        const auto zero = Clock::time_point{};
        // Every small cardinality exercises coincident percentile ranks and
        // rounding boundaries, with ties, monotone inputs and an isolated spike.
        for (std::size_t size = 0; size <= 350; ++size) {
            for (int pattern = 0; pattern < 7; ++pattern) {
                std::vector<PerformanceSample> delays, drifts;
                for (std::size_t i = 0; i < size; ++i) {
                    const auto value = pattern == 0   ? 0.0
                                       : pattern == 1 ? static_cast<double>(i)
                                       : pattern == 2 ? static_cast<double>(size - i)
                                       : pattern == 3 ? (i == size / 2 ? 1000000.0 : 1.0)
                                       : pattern == 5 ? static_cast<double>(i % 8)
                                       : pattern == 6 ? static_cast<double>(i % 9)
                                                      : static_cast<double>(random() % 97) / 16;
                    // Equal timestamps model a delayed executor catching up.
                    delays.push_back({zero + Millis(i / 3), value});
                    if (i % 7 == 0)
                        drifts.push_back(delays.back());
                }
                for (const auto cutoff :
                     {zero - Millis(1), zero, zero + Millis(size / 6), zero + Millis(size / 3 + 1)})
                    check(delays, drifts, cutoff, scratch);
            }
        }
        std::vector<PerformanceSample> delays, drifts;
        for (std::size_t i = 0; i < 15000; ++i) {
            delays.push_back({zero + Millis(i * 20), static_cast<double>(random() % 100003) / 16});
            if (i % 50 == 0)
                drifts.push_back(delays.back());
        }
        scratch.reserve(delays.size());
        const auto capacity = scratch.capacity();
        const auto* storage = scratch.data();
        for (const auto now : {zero + Millis(299980), zero + Millis(300000), zero + Millis(600001)})
            for (const auto seconds : {10, 60, 300}) {
                check(delays, drifts, now - std::chrono::seconds(seconds), scratch);
                require(scratch.capacity() == capacity && scratch.data() == storage,
                        "capture windows must reuse the caller's reserved scratch buffer");
            }
        // Overflow the small frequency table only near the end. General
        // selection must still include the long repeated-value prefix.
        for (auto& sample : delays)
            sample.ms = 0;
        for (std::size_t i = 1; i <= 9; ++i)
            delays[delays.size() - i].ms = static_cast<double>(i);
        check(delays, drifts, zero, scratch);
        std::cout << "Exact percentiles, empty/coincident ranks, inclusive cutoff, expiry and scratch reuse "
                     "passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
