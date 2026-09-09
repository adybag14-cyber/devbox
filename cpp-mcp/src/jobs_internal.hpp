#pragma once
#include "devbox/jobs.hpp"
#include <map>
namespace devbox {
struct JobStore::Maintenance {
    struct Entry {
        std::uint64_t bytes = 0;
        bool terminal = false;
        std::int64_t completed = 0;
    };
    std::mutex mutex;
    std::vector<std::string> ids;
    std::size_t cursor = 0;
    std::uint64_t cycle = 0;
    std::optional<Clock::time_point> cycle_started, refreshed, quota_checked, quota_refreshed;
    std::map<std::string, Entry> quota_entries;
    bool quota_initialized = false;
    std::uint64_t store_bytes = 0, terminal_retained = 0;
    bool quota_pressure = false;
    std::string quota_checked_utc;
};
} // namespace devbox
