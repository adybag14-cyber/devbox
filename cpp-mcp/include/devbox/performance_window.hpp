#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace devbox::detail {
struct PerformanceSample {
    std::chrono::steady_clock::time_point at;
    double ms;
    bool operator==(const PerformanceSample&) const = default;
};

// The monitor serializes access. Storage is bounded and allocated once; taking
// a snapshot preserves the chronological order used by its existing windows.
class PerformanceWindow {
    std::vector<PerformanceSample> samples_;
    std::size_t first_ = 0, size_ = 0;

    void remove_first() {
        if (++first_ == samples_.size())
            first_ = 0;
        --size_;
    }

  public:
    explicit PerformanceWindow(std::size_t capacity) : samples_(capacity) {
        if (capacity == 0)
            throw std::invalid_argument("Performance window capacity must be positive");
    }

    void push(PerformanceSample sample) {
        auto next = first_ + size_;
        if (next >= samples_.size())
            next -= samples_.size();
        samples_[next] = sample;
        if (size_ == samples_.size())
            remove_first();
        ++size_;
        // The oldest sample exactly five minutes ago is still included.
        while (size_ && sample.at - samples_[first_].at > std::chrono::minutes(5))
            remove_first();
    }

    void copy_to(std::vector<PerformanceSample>& result) const {
        result.clear();
        result.reserve(size_);
        const auto first_count = std::min(size_, samples_.size() - first_);
        result.insert(result.end(), samples_.data() + first_, samples_.data() + first_ + first_count);
        result.insert(result.end(), samples_.data(), samples_.data() + size_ - first_count);
    }
};
} // namespace devbox::detail
