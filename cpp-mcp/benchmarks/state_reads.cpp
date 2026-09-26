// Same-binary paired native query timing; fixtures only, no server or production settings.
#include "devbox/state_store.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
using namespace devbox;
namespace {
using Timer = std::chrono::steady_clock;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
struct Fixture {
    std::shared_ptr<StateStore> state;
    explicit Fixture(const fs::path& root, bool reuse) {
        StateStoreOptions options;
        options.reuse_read_statements = reuse;
        state = open_state_store(root, options);
        std::vector<StateMutation> rows;
        for (unsigned i = 0; i < 240; ++i)
            rows.push_back({{"job", "row-" + std::to_string(1000 + i), "owner-" + std::to_string(i % 4),
                             "group-" + std::to_string(i % 3), ((i % 2) != 0) ? "running" : "done", 0,
                             Json{{"i", i}, {"payload", std::string(128, 'x')}}},
                            0});
        rows.push_back(
            {{"blob", "four-k", "owner", "group", "ready", 0, Json{{"payload", std::string(4096, 'x')}}}, 0});
        std::vector<StateEvent> events;
        for (unsigned i = 0; i < 10; ++i)
            events.push_back({"run-one", "recorded", 0, Json{{"i", i}}});
        state->apply(rows, events);
    }
};
Json measure(const std::string& name, unsigned samples, const std::function<std::uint64_t()>& fn) {
    for (unsigned i = 0; i < 64; ++i)
        (void)fn();
    std::vector<double> us;
    us.reserve(samples);
    std::uint64_t checksum = 0;
    for (unsigned i = 0; i < samples; ++i) {
        const auto start = Timer::now();
        const auto value = fn();
        const auto elapsed = Timer::now() - start;
        checksum += value;
        us.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
    }
    std::sort(us.begin(), us.end());
    double sum = 0;
    for (const auto value : us)
        sum += value;
    const auto percentile = [&](unsigned p) { return us[(us.size() - 1) * p / 100]; };
    return Json{{"operation", name},       {"samples", samples},    {"unit", "microseconds"},
                {"p50", percentile(50)},   {"p95", percentile(95)}, {"p99", percentile(99)},
                {"mean", sum / us.size()}, {"min", us.front()},     {"max", us.back()},
                {"checksum", checksum}};
}
Json trial(Fixture& fixture, unsigned samples) {
    auto& store = fixture.state;
    StateQuery page{"job", "owner-1", "group-1", "running", {}, 10};
    StateCountQuery count{"job", "owner-1", "group-1", {"running", "done", "running"}, 16};
    Json rows = Json::array();
    rows.push_back(measure("clock_pair_floor", samples, [] { return 1; }));
    rows.push_back(measure("get_hit_128b", samples, [&] {
        const auto row = store->get("job", "row-1001");
        require(row && row->data["i"] == 1 && row->principal == "owner-1", "get oracle");
        return row->revision;
    }));
    rows.push_back(measure("get_miss", samples, [&] {
        require(!store->get("job", "missing"), "missing oracle");
        return 1;
    }));
    rows.push_back(measure("get_hit_4k", samples, [&] {
        const auto row = store->get("blob", "four-k");
        require(row && row->data["payload"].get_ref<const std::string&>().size() == 4096, "4k oracle");
        return row->revision;
    }));
    rows.push_back(measure("count_total", samples, [&] {
        const auto value = store->count("job");
        require(value == 240, "count oracle");
        return value;
    }));
    rows.push_back(measure("count_owner", samples, [&] {
        const auto value = store->count("job", "owner-1");
        require(value == 60, "owner count oracle");
        return value;
    }));
    rows.push_back(measure("count_matching_capped", samples, [&] {
        const auto value = store->count_matching(count);
        require(value == 16, "capped count oracle");
        return value;
    }));
    rows.push_back(measure("list_filtered_10", samples, [&] {
        const auto result = store->list(page);
        require(result.records.size() == 10 && result.next.has_value(), "page oracle");
        return result.records.size();
    }));
    rows.push_back(measure("events_10", samples, [&] {
        const auto result = store->events("run-one", 0, 10);
        require(result.size() == 10 && result.back().sequence == 10, "event oracle");
        return result.size();
    }));
    const unsigned per_thread = samples / 4;
    std::atomic<unsigned> failures = 0;
    const auto start = Timer::now();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t)
        workers.emplace_back([&] {
            try {
                for (unsigned i = 0; i < per_thread; ++i)
                    if (store->count("job", "owner-1") != 60)
                        ++failures;
            } catch (...) {
                ++failures;
            }
        });
    for (auto& worker : workers)
        worker.join();
    const auto total_us = std::chrono::duration<double, std::micro>(Timer::now() - start).count();
    require(failures == 0, "contended count oracle");
    rows.push_back(Json{{"operation", "count_owner_four_threads"},
                        {"unit", "microseconds"},
                        {"samples", per_thread * 4},
                        {"amortized_us", total_us / (per_thread * 4)},
                        {"includes_thread_start_join", true}});
    return rows;
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc >= 1 && argc <= 3, "Usage: devbox-state-read-bench [NEW_FIXTURE [SAMPLES]]");
        const bool smoke = argc == 1;
        const auto root = smoke ? fs::canonical(fs::temp_directory_path()) / ("devbox-state-bench-" + uuid())
                                : fs::absolute(path_from_utf8(argv[1]));
        require(!fs::exists(root), "benchmark needs a new fixture directory");
        const auto samples = argc == 3 ? std::stoul(argv[2]) : smoke ? 100UL : 2000UL;
        require(samples >= 100 && samples <= 20000, "sample bound 100..20000");
        ensure_private_state_directory(root);
        ScopeExit cleanup([&] {
            if (smoke) {
                std::error_code ec;
                fs::remove_all(root, ec);
            }
        });
        Fixture baseline(root / "uncached", false), candidate(root / "cached", true);
        Json report{{"schema", 1},
                    {"timer", "std::chrono::steady_clock"},
                    {"timing_scope",
                     "in-process native SQLite queries including result construction and oracle checks"},
                    {"transport_included", false},
                    {"same_binary", true},
                    {"durability", "full"},
                    {"mode_order", "ABBAABBA"},
                    {"production_requests", 0},
                    {"trials", Json::array()}};
        for (unsigned round = 0; round < 8; ++round) {
            const bool reuse = round % 4 == 1 || round % 4 == 2;
            report["trials"].push_back(
                Json{{"round", round},
                     {"reuse", reuse},
                     {"operations", trial(reuse ? candidate : baseline, static_cast<unsigned>(samples))}});
        }
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
