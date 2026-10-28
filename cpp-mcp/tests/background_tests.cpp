#include "devbox/background.hpp"
#include "devbox/allocator.hpp"
#include "devbox/scoped_thread.hpp"
#include <array>
#include <iostream>

using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
void shape(const Json& value) {
    const std::array<std::string_view, 14> keys{"kind",
                                                "running",
                                                "starts",
                                                "restarts",
                                                "lastTickUnixMs",
                                                "lastAttemptUnixMs",
                                                "lastSuccessUnixMs",
                                                "lastFailureUnixMs",
                                                "consecutiveFailures",
                                                "lastError",
                                                "lastTickAgeMs",
                                                "lastSuccessAgeMs",
                                                "lastFailureAgeMs",
                                                "idleForMs"};
    require(value.is_object() && value.size() == keys.size(), "Background wire fields changed");
    std::size_t i = 0;
    for (const auto& item : value.items())
        require(item.key() == keys.at(i++), "Background wire field ordering changed");
}
void ages(const Json& value, std::uint64_t before, std::uint64_t after) {
    for (const auto& [timestamp, age] :
         std::array<std::pair<const char*, const char*>, 3>{{{"lastTickUnixMs", "lastTickAgeMs"},
                                                             {"lastSuccessUnixMs", "lastSuccessAgeMs"},
                                                             {"lastFailureUnixMs", "lastFailureAgeMs"}}}) {
        if (value[timestamp].is_null()) {
            require(value[age].is_null(), "Absent event gained a fabricated age");
        } else {
            const auto at = json_uint(value, timestamp), elapsed = json_uint(value, age);
            require(elapsed >= before - std::min(before, at) && elapsed <= after - std::min(after, at),
                    "Event age is outside the snapshot clock bounds");
        }
    }
}
} // namespace
int main() {
    try {
        BackgroundTasks background;
        require(background.snapshot() == Json::object(), "Unused background registry is not empty");
        require(!background.healthy("missing", 150000) && background.snapshot().empty(),
                "A readiness lookup must not create a missing worker");
        background.mark_stopped("z-unstarted");
        const auto unstarted = background.snapshot()["z-unstarted"];
        require(
            unstarted.dump() ==
                R"({"kind":"","running":false,"starts":0,"restarts":0,"lastTickUnixMs":null,"lastAttemptUnixMs":null,"lastSuccessUnixMs":null,"lastFailureUnixMs":null,"consecutiveFailures":0,"lastError":null,"lastTickAgeMs":null,"lastSuccessAgeMs":null,"lastFailureAgeMs":null,"idleForMs":null})",
            "Initial state no longer matches the established JSON contract");
        background.mark_started("a-events");
        require(!background.healthy("a-events", 150000), "Starting does not establish a successful probe");
        auto value = background.snapshot()["a-events"];
        shape(value);
        require(value["kind"] == "event-driven" && value["starts"] == 1 && value["restarts"] == 0 &&
                    value["running"] == true && value["idleForMs"].is_number_unsigned() &&
                    value["lastSuccessUnixMs"].is_null(),
                "Event worker startup fabricates success or loses idle state");
        const auto first_attempt = value["lastAttemptUnixMs"];
        const std::string error = "An error longer than small-string storage with an escaped \"quote\"";
        background.failure("a-events", error);
        background.failure("a-events", "second failure");
        value = background.snapshot()["a-events"];
        require(value["consecutiveFailures"] == 2 && value["lastError"] == "second failure" &&
                    value["lastAttemptUnixMs"] == first_attempt && value["lastSuccessUnixMs"].is_null(),
                "Failures must count without inventing attempts or successes");
        const auto failure_time = value["lastFailureUnixMs"];
        background.mark_stopped("a-events");
        value = background.snapshot()["a-events"];
        require(value["running"] == false && value["lastError"] == "second failure" &&
                    value["consecutiveFailures"] == 2,
                "Stopping must preserve an existing failure");
        background.mark_started("a-events", "periodic");
        background.attempt("a-events");
        background.success("a-events");
        require(background.healthy("a-events", 150000) && !background.healthy("a-events", 0),
                "A successful running worker requires a strictly positive freshness allowance");
        const std::string bounded_name = "a-events-suffix";
        require(background.healthy(std::string_view(bounded_name).substr(0, 8), 150000) &&
                    !background.healthy(bounded_name, 150000),
                "Readiness lookup must respect the exact string-view boundary");
        const auto allocations_before = allocator_counters();
        for (unsigned check = 0; check < 1000; ++check)
            if (!background.healthy("a-events", 150000))
                throw Error("Stable readiness unexpectedly changed");
        const auto allocations_after = allocator_counters();
        require(allocations_after.calls == allocations_before.calls,
                "Typed background readiness must not allocate diagnostic objects");
        const auto before = unix_millis();
        auto detached_snapshot = background.snapshot();
        const auto after = unix_millis();
        value = detached_snapshot["a-events"];
        shape(value);
        ages(value, before, after);
        require(value["starts"] == 2 && value["restarts"] == 1 && value["kind"] == "periodic" &&
                    value["running"] == true && value["idleForMs"].is_null() &&
                    value["lastTickUnixMs"] == value["lastSuccessUnixMs"] &&
                    value["lastFailureUnixMs"] == failure_time && value["consecutiveFailures"] == 0 &&
                    value["lastError"].is_null(),
                "Restart and recovery must retain failure history while clearing current errors");
        require(detached_snapshot.begin().key() == "a-events", "Worker names must remain sorted");
        detached_snapshot["a-events"]["starts"] = 99;
        require(background.snapshot()["a-events"]["starts"] == 2, "Snapshots must own detached values");
        background.mark_stopped("a-events", error);
        require(!background.healthy("a-events", 150000), "A stopped or failed worker is not ready");
        value = background.snapshot()["a-events"];
        require(value["running"] == false && value["lastError"] == error && value["consecutiveFailures"] == 1,
                "Terminal failure must retain its exact text and stopped status");
        background.success("age-check");
        std::this_thread::sleep_for(Millis(10));
        require(!background.healthy("age-check", 1), "An expired successful probe is not ready");
        background.failure("age-check", "failed after success");
        require(!background.healthy("age-check", 150000), "A recent success must not hide a later failure");
        background.success("age-check");
        require(background.healthy("age-check", 150000), "A later successful probe must restore readiness");

        // Multiple workers update the same diagnostic name while snapshots are
        // collected. Exact totals catch lost updates; snapshots must remain valid.
        background.mark_started("concurrent");
        std::atomic<unsigned> finished{0};
        std::vector<ScopedThread> writers;
        for (unsigned worker = 0; worker < 4; ++worker)
            writers.emplace_back([&] {
                for (unsigned n = 0; n < 2000; ++n)
                    background.failure("concurrent", error);
                ++finished;
            });
        while (finished < 4) {
            require(!background.healthy("concurrent", 150000),
                    "Concurrent failures must not fabricate successful readiness");
            const auto concurrent = background.snapshot()["concurrent"];
            shape(concurrent);
            require(json_uint(concurrent, "consecutiveFailures") <= 8000,
                    "Concurrent snapshot exposed an invalid failure count");
            std::this_thread::yield();
        }
        writers.clear();
        require(background.snapshot()["concurrent"]["consecutiveFailures"] == 8000,
                "Concurrent updates lost failure counts");
        std::cout
            << "Background state contract, lifecycle, snapshot ownership and concurrent updates passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
