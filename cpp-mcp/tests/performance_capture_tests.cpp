#include "devbox/allocator.hpp"
#include "devbox/native.hpp"
#include "devbox/telemetry.hpp"
#include <boost/asio/io_context.hpp>
#include <iostream>

using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
} // namespace
int main() {
    const auto temporary = fs::weakly_canonical(fs::temp_directory_path());
    const auto root = temporary / path_from_utf8("devbox-capture-memory-" + uuid());
    fs::create_directory(root);
    ScopeExit cleanup([&] {
        std::error_code error;
        if (!fs::is_symlink(root) && fs::weakly_canonical(root).parent_path() == temporary)
            fs::remove_all(root, error);
    });
    try {
        Config config;
        config.project_root = root;
        config.mcp_performance_state_path = root / "performance.json";
        BackgroundTasks background;
        PerformanceMonitor monitor(config, background, [] { return Json{{"gitSha", "fixture"}}; });
        asio::io_context io(1);
        auto work = asio::make_work_guard(io);
        auto cancel = std::make_shared<Cancellation>();
        monitor.attach(io.get_executor(), cancel);
        std::thread sampler([&] { io.run(); });
        ScopeExit stop([&] {
            cancel->cancel();
            work.reset();
            if (sampler.joinable())
                sampler.join();
            background.stop();
        });
        const auto deadline = Clock::now() + std::chrono::seconds(45);
        std::uint64_t count = 0;
        do {
            std::this_thread::sleep_for(Millis(500));
            monitor.persist();
            count = json_uint(monitor.snapshot()["eventLoop"]["fiveMinute"], "sampleCount");
            require(Clock::now() < deadline, "Real sampler did not produce the bounded fixture history");
        } while (count < 1500);
        cancel->cancel();
        work.reset();
        sampler.join();
        background.stop();
        stop.disarm();

        // Freeze both sampler and persistence workers. The independent allocator
        // counter does not allocate; no concurrent work can explain a history-
        // sized excess in the reported live-memory figure.
        monitor.persist();
        const auto frozen_count = json_uint(monitor.snapshot()["eventLoop"]["fiveMinute"], "sampleCount");
        monitor.persist();
        const auto before_copy = allocator_counters();
        const auto snapshot = monitor.snapshot();
        const auto after_copy = allocator_counters();
        const auto reported = json_uint(snapshot["process"]["memory"]["allocator"], "currentRequestedBytes");
        require(after_copy.current >= before_copy.current, "Snapshot copy must retain its own JSON storage");
        const auto copy_bytes = after_copy.current - before_copy.current;
        const auto excess = reported > after_copy.current ? reported - after_copy.current : 0;
        std::cout << Json{{"samples", frozen_count},
                          {"reportedRequestedBytes", reported},
                          {"independentLiveBytes", after_copy.current},
                          {"snapshotCopyBytes", copy_bytes},
                          {"reportedExcessBytes", excess}}
                         .dump()
                  << '\n';
        // Allow a whole extra result-sized temporary for JSON construction, but
        // never the copied sampling histories and percentile scratch workspace.
        if (json_string(snapshot["process"]["memory"]["allocator"], "backend").starts_with("cpp-global-new"))
            require(excess <= copy_bytes, "Live allocation telemetry includes discarded capture workspace");
        require(json_uint(snapshot["eventLoop"]["fiveMinute"], "sampleCount") == frozen_count,
                "The frozen sample history changed during measurement");
        std::cout << "Real sampling, quiescent capture and independent live-allocation comparison passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
