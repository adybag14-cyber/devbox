#pragma once
#include "performance_window.hpp"
#include <array>
#include <span>

namespace devbox::detail {
struct PerformanceStatistics {
    std::size_t count = 0;
    double p50 = 0, p95 = 0, p99 = 0, maximum = 0, drift_maximum = 0;
    bool operator==(const PerformanceStatistics&) const = default;
};

// Samples are chronological and delay values are finite. The caller owns the
// scratch storage and may reuse it across the 10/60/300-second capture windows.
inline PerformanceStatistics performance_statistics(std::span<const PerformanceSample> delays,
                                                    std::span<const PerformanceSample> drifts,
                                                    std::chrono::steady_clock::time_point cutoff,
                                                    std::vector<double>& scratch) {
    const auto since = [cutoff](std::span<const PerformanceSample> samples) {
        return std::lower_bound(samples.begin(), samples.end(), cutoff,
                                [](const PerformanceSample& sample, auto at) { return sample.at < at; });
    };
    PerformanceStatistics result;
    struct Frequency {
        double value = 0;
        std::size_t count = 0;
    };
    // Repeated delays (for example zero plus occasional stalls) are common in
    // synthetic/quiet traces. An exact small frequency table avoids partitioning
    // those ties repeatedly. More distinct values disable it after at most nine
    // insertions; all values are still retained for the general selection path.
    std::array<Frequency, 8> frequencies{};
    std::size_t distinct = 0;
    bool frequencies_complete = true;
    const auto first = since(delays);
    scratch.clear();
    scratch.reserve(static_cast<std::size_t>(delays.end() - first));
    for (auto it = first; it != delays.end(); ++it) {
        result.maximum = scratch.empty() ? it->ms : std::max(result.maximum, it->ms);
        scratch.push_back(it->ms);
        if (frequencies_complete) {
            std::size_t index = 0;
            while (index < distinct && frequencies[index].value != it->ms)
                ++index;
            if (index < distinct)
                ++frequencies[index].count;
            else if (distinct < frequencies.size())
                frequencies[distinct++] = {it->ms, 1};
            else
                frequencies_complete = false;
        }
    }
    for (auto it = since(drifts); it != drifts.end(); ++it)
        result.drift_maximum = std::max(result.drift_maximum, it->ms);
    result.count = scratch.size();
    if (scratch.empty())
        return result;

    std::array<double, 3> selected{};
    constexpr std::array<std::size_t, 3> percentiles{50, 95, 99};
    if (frequencies_complete)
        std::sort(frequencies.begin(), frequencies.begin() + static_cast<std::ptrdiff_t>(distinct),
                  [](const Frequency& left, const Frequency& right) { return left.value < right.value; });
    auto remaining = scratch.begin();
    std::size_t previous = 0;
    for (std::size_t i = 0; i < percentiles.size(); ++i) {
        const auto n = scratch.size() - 1, p = percentiles[i];
        // Exactly round((count-1)*p/100), without an intermediate size overflow.
        const auto index = n / 100 * p + (n % 100 * p + 50) / 100;
        if (frequencies_complete) {
            std::size_t cumulative = 0;
            for (std::size_t j = 0; j < distinct; ++j) {
                cumulative += frequencies[j].count;
                if (index < cumulative) {
                    selected[i] = frequencies[j].value;
                    break;
                }
            }
            continue;
        }
        if (i && index == previous) {
            selected[i] = selected[i - 1];
            continue;
        }
        auto nth = scratch.begin() + static_cast<std::ptrdiff_t>(index);
        std::nth_element(remaining, nth, scratch.end());
        selected[i] = *nth;
        remaining = nth + 1;
        previous = index;
    }
    result.p50 = selected[0];
    result.p95 = selected[1];
    result.p99 = selected[2];
    return result;
}
} // namespace devbox::detail
