#include "devbox/scheduler.hpp"
#include "devbox/scheduler_notifications.hpp"
#include <algorithm>
#include <iostream>
using namespace devbox;
namespace {
std::uint64_t monotonic_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
SchedulerConfig config_at(const fs::path& root) {
    SchedulerConfig config;
    config.root = root;
    config.max_concurrent = 1;
    config.reserved_interactive = 0;
    config.queue_timeout = Millis(5000);
    return config;
}
int run(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--notified-child") {
        ExecutionScheduler scheduler(config_at(path_from_utf8(argv[2])));
        auto waiter = scheduler.begin({});
        bool ready = false;
        for (;;) {
            auto changed = waiter.changed_token();
            if (auto lease = waiter.poll()) {
                require(ready, "qualification must actually wait behind the parent's lease");
                std::cout << Json{{"acquired_steady_us", monotonic_us()}}.dump() << '\n' << std::flush;
                return 0;
            }
            if (!ready) {
                std::cout << "READY\n" << std::flush;
                ready = true;
            }
            (void)changed->wait_for(waiter.poll_interval());
        }
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-notification-" + uuid());
    fs::create_directory(root);
    ScopeExit clean([&] {
        std::error_code error;
        fs::remove_all(root, error);
    });
    try {
        const std::size_t count = argc == 2 && std::string_view(argv[1]) == "--qualify" ? 1000 : 12;
        ExecutionScheduler scheduler(config_at(root / "slots"));
        require(scheduler.snapshot()["notifications"]["available"] == true,
                "native owner-directory notification available");
        std::vector<double> samples;
        for (std::size_t i = 0; i < count; ++i) {
            auto held = scheduler.acquire({});
            std::string pending;
            std::uint64_t released_at = 0;
            ProcessOptions options;
            options.timeout = Millis(7000);
            options.max_capture_chars = 4096;
            options.on_output = [&](OutputStream stream, std::string_view bytes) {
                if (stream != OutputStream::stdout_stream)
                    return;
                pending.append(bytes);
                if (!released_at && pending.find("READY") != std::string::npos) {
                    released_at = monotonic_us();
                    held.release();
                }
            };
            const auto output = spawn_process(path_text(executable_path()),
                                              {"--notified-child", path_text(root / "slots")}, options);
            const auto object = output.stdout_text.find('{');
            require(released_at && object != std::string::npos,
                    "owned child readiness and grant acknowledgement");
            const auto acquired =
                json_uint(Json::parse(output.stdout_text.substr(object)), "acquired_steady_us");
            require(acquired >= released_at, "paired cross-process steady timestamps ordered");
            samples.push_back(static_cast<double>(acquired - released_at) / 1000.0);
        }
        std::sort(samples.begin(), samples.end());
        const auto p95 = samples[std::min(count - 1, count * 95 / 100)];
        // Hosted shared runners establish correctness only; the explicit controlled run is the performance
        // gate.
        require(p95 < (count == 1000 ? 20.0 : 1500.0), "notification-to-grant qualification bound");
        require(scheduler.snapshot()["occupied"] == 0 && scheduler.snapshot()["global_queued"] == 0,
                "no double lease or abandoned queue ticket after repeated cross-process notifications");
        std::cout << Json{{"samples", count},
                          {"p50_ms", samples[count / 2]},
                          {"p95_ms", p95},
                          {"p99_ms", samples[std::min(count - 1, count * 99 / 100)]},
                          {"max_ms", samples.back()},
                          {"metric", "held_lease_release_to_cross_process_grant"},
                          {"notifications", scheduler.snapshot()["notifications"]}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    std::vector<std::string> values;
    for (int i = 0; i < argc; ++i)
        values.push_back(narrow(wide_argv[i]));
    std::vector<char*> args;
    for (auto& value : values)
        args.push_back(value.data());
    return run(argc, args.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
