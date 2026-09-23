#pragma once
#include <cstdint>
namespace devbox {
struct AllocatorCounters {
    std::uint64_t current, peak, allocated, freed, active, calls, deletes;
};
// Independent exact counters, sampled without allocating. Concurrent snapshots
// are observations, not a transaction across all fields. No events are sampled away.
AllocatorCounters allocator_counters() noexcept;
} // namespace devbox
