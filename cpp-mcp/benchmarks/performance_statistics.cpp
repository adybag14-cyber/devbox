#include "devbox/performance_statistics.hpp"
#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include <iostream>

using namespace devbox;
using detail::PerformanceSample;
using detail::PerformanceStatistics;
namespace {
// The deployed full-sort algorithm is retained only as a measured reference.
PerformanceStatistics sorted_statistics(const std::vector<PerformanceSample>& delays,
                                        const std::vector<PerformanceSample>& drifts,
                                        Clock::time_point cutoff) {
    std::vector<double> values;
    PerformanceStatistics result;
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
std::uint64_t measure(bool select, int round, bool spikes) {
    const auto before = allocator_counters();
    const auto began = Clock::now();
    detail::PerformanceWindow delays(15000), drifts(301);
    std::uint64_t checksum = 0;
    for (std::int64_t i = 0; i < 180000; ++i) {
        const auto at = Clock::time_point{} + Millis(i * 20);
        const auto value = spikes ? (i % 497 == 0 ? 1000.0 : static_cast<double>(i % 5) / 16)
                                  : static_cast<double>((i * 7919) % 241) / 16;
        delays.push({at, value});
        if (i % 50 == 0)
            drifts.push({at, value});
        if (i % 500 != 0)
            continue;
        std::vector<PerformanceSample> d, r;
        delays.copy_to(d);
        drifts.copy_to(r);
        std::vector<double> scratch;
        if (select)
            scratch.reserve(d.size());
        for (const auto seconds : {10, 60, 300}) {
            const auto cutoff = at - std::chrono::seconds(seconds);
            const auto result = select ? detail::performance_statistics(d, r, cutoff, scratch)
                                       : sorted_statistics(d, r, cutoff);
            for (const auto number :
                 {result.p50, result.p95, result.p99, result.maximum, result.drift_maximum})
                checksum = checksum * 31 + static_cast<std::uint64_t>(number * 16);
            checksum = checksum * 31 + result.count;
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    const auto after = allocator_counters();
    std::cout << Json{{"variant", select ? "selection" : "sort"},
                      {"distribution", spikes ? "ties-and-spikes" : "quantized"},
                      {"round", round},
                      {"simulated_ticks", 180000},
                      {"captures", 360},
                      {"elapsed_ms", elapsed},
                      {"allocation_calls", after.calls - before.calls},
                      {"allocated_bytes", after.allocated - before.allocated},
                      {"checksum", std::to_string(checksum)}}
                     .dump()
              << '\n'
              << std::flush;
    return checksum;
}
} // namespace
int main() {
    try {
        std::cout << Json{{"benchmark", "performance-statistics"},
                          {"build", build_snapshot()},
                          {"method", "Three ABBA cycles per distribution. Identical production ring "
                                     "histories and 360 captures per accelerated one-hour trace, matching "
                                     "the steady ten-second persistence interval. Exact "
                                     "full-sort/selection checksums must match. Component evidence only."}}
                         .dump()
                  << '\n';
        for (const bool spikes : {false, true}) {
            std::optional<std::uint64_t> expected;
            for (int round = 0; round < 3; ++round)
                for (const bool select : {false, true, true, false}) {
                    const auto value = measure(select, round, spikes);
                    if (expected && *expected != value)
                        throw Error("Percentile benchmark checksum mismatch");
                    expected = value;
                }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
