#include "devbox/scheduler.hpp"
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class F> void rejects(F&& operation, std::string_view part) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(part) != std::string_view::npos)
            return;
        throw Error("Unexpected error: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(part));
}
AcquireRequest background(std::string label, ResourceClass resource = ResourceClass::light,
                          std::size_t weight = 1) {
    return {ExecutionKind::background, resource, weight, std::move(label), {}};
}
int run(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--contend") {
        SchedulerConfig config;
        config.root = path_from_utf8(argv[2]);
        config.max_concurrent = 3;
        config.reserved_interactive = 1;
        config.heavy_capacity = 2;
        config.queue_timeout = Millis(10000);
        try {
            ExecutionScheduler scheduler(config);
            for (int i = 0; i < 5; ++i) {
                auto lease = scheduler.acquire(background("process-contender", ResourceClass::heavy, 2));
                require(scheduler.snapshot()["occupied"].get<std::size_t>() <= 3, "process oversubscription");
                std::this_thread::sleep_for(Millis(15));
            }
        } catch (const std::exception& error) {
            std::cerr << "Contender " << process_id() << ": " << error.what() << '\n';
            return 1;
        }
        return 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-scheduler-" + uuid());
    fs::create_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        SchedulerConfig config;
        config.root = root / "basic";
        config.max_concurrent = 3;
        config.reserved_interactive = 1;
        config.heavy_capacity = 2;
        config.watch_max_concurrent = 2;
        config.queue_timeout = Millis(300);
        ExecutionScheduler scheduler(config);
        auto heavy = scheduler.acquire(background("heavy", ResourceClass::heavy, 2));
        require(heavy.slots == std::vector<std::size_t>{0, 1}, "weighted slots");
        auto foreground =
            scheduler.acquire({ExecutionKind::interactive, ResourceClass::light, 1, "interactive", {}});
        require(foreground.slots == std::vector<std::size_t>{2}, "reserved interactive slot");
        auto watch = scheduler.acquire(background("watch", ResourceClass::watch, 10));
        require(watch.pool == "watch" && watch.weight == 1, "independent watch pool");
        rejects([&] { scheduler.acquire(background("saturated")); }, "remained saturated");
        require(scheduler.snapshot()["global_queued"] == 0, "timeout removes queue ticket");
        auto cancel = std::make_shared<Cancellation>();
        auto waiting = scheduler.begin(background("cancelled"));
        require(!waiting.poll(cancel), "pending queue poll");
        cancel->cancel();
        rejects([&] { waiting.poll(cancel); }, "queue wait cancelled");
        // Destroying a cancelled waiter releases its durable ticket.
        waiting = scheduler.begin(background("abandoned"));
        heavy.release();
        foreground.release();
        watch.release();
        require(scheduler.snapshot()["occupied"] == 0 && scheduler.snapshot()["watch_occupied"] == 0,
                "lease release");
        const auto active_identity = std::to_string(*process_instance(process_id()));
        write_json_atomic(config.root / "slot-00.json",
                          Json{{"pid", process_id()},
                               {"processInstance", std::to_string(*process_instance(process_id()) + 1)},
                               {"token", "stale"}});
        auto stale = scheduler.acquire(background("reclaimed"));
        require(stale.slots.front() == 0, "stale identity reclaim");
        stale.release();
        write_json_atomic(
            config.root / "slot-00.json",
            Json{{"pid", process_id()}, {"processInstance", active_identity}, {"token", "preserve-live"}});
        auto other = scheduler.acquire(background("skip-live"));
        require(other.slots.front() == 1 &&
                    read_json(config.root / "slot-00.json")["token"] == "preserve-live",
                "live identity preserved");
        other.release();
#ifdef _WIN32
        // Model a transient Windows reader/rename sharing conflict. Inspection
        // must retain live ownership once the conflict clears, and persistent
        // denial must remain an error rather than making the slot reclaimable.
        const auto lock_live_slot = [&] {
            NativeHandle handle(CreateFileW((config.root / "slot-00.json").c_str(), GENERIC_READ, 0, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            require(static_cast<bool>(handle), "lock live slot fixture");
            return handle;
        };
        auto reader = std::async(std::launch::async, [handle = lock_live_slot()]() mutable {
            std::this_thread::sleep_for(Millis(30));
            handle.reset();
        });
        require(scheduler.snapshot()["occupied"] == 1, "transient sharing retains live ownership");
        reader.get();
        {
            auto denied = lock_live_slot();
            rejects([&] { scheduler.snapshot(); }, "inspect scheduler owner");
        }
        require(read_json(config.root / "slot-00.json")["token"] == "preserve-live",
                "persistent sharing denial cannot reclaim a live slot");
#endif
        fs::remove(config.root / "slot-00.json");
        std::cout << "PASS weighted admission, reserved capacity, watch pool, cancellation and identities\n"
                  << std::flush;
        SchedulerConfig fifo_config = config;
        fifo_config.root = root / "fifo";
        fifo_config.max_concurrent = 1;
        fifo_config.reserved_interactive = 0;
        fifo_config.queue_timeout = Millis(3000);
        ExecutionScheduler fifo(fifo_config);
        auto blocker = fifo.acquire(background("blocker"));
        auto first = fifo.begin(background("first"));
        require(!first.poll(), "first queued");
        auto second = fifo.begin(background("second"));
        require(!second.poll(), "second queued");
        blocker.release();
        {
            auto newcomer = fifo.begin(background("newcomer"));
            require(!newcomer.poll(), "immediate admission cannot bypass a real queued ticket");
        }
        require(!second.poll(), "later queue ticket cannot overtake");
        auto first_lease = first.poll();
        require(first_lease.has_value(), "queue head acquisition");
        first_lease->release();
        auto second_lease = second.poll();
        require(second_lease.has_value(), "queue promotion");
        second_lease->release();
        SchedulerConfig pressure_config = config;
        pressure_config.root = root / "pressure";
        pressure_config.max_concurrent = 6;
        pressure_config.heavy_capacity = 5;
        pressure_config.io_heavy_capacity = 2;
        fs::create_directory(pressure_config.root);
        write_json_atomic(pressure_config.root / ".disk-pressure.json", Json{{"diskPressure", "warning"}});
        ExecutionScheduler pressure(pressure_config);
        auto light =
            pressure.acquire({ExecutionKind::interactive, ResourceClass::light, 1, "light-pressure", {}});
        require(light.slots.front() == 2, "disk pressure protects low weighted slots");
        auto weighted = pressure.acquire(background("weighted-pressure", ResourceClass::heavy, 2));
        require(weighted.slots == std::vector<std::size_t>{0, 1}, "weighted work progresses under pressure");
        weighted.release();
        light.release();
        SchedulerConfig age_config = fifo_config;
        age_config.root = root / "aging";
        age_config.background_priority_age = Millis(40);
        ExecutionScheduler aging(age_config);
        auto occupied = aging.acquire({ExecutionKind::interactive, ResourceClass::light, 1, "occupied", {}});
        auto older = aging.begin(background("older"));
        require(!older.poll(), "older background queued");
        std::this_thread::sleep_for(Millis(70));
        occupied.release();
        auto interactive =
            aging.begin({ExecutionKind::interactive, ResourceClass::light, 1, "new-interactive", {}});
        require(!interactive.poll(), "aged background receives priority");
        auto older_lease = older.poll();
        require(older_lease.has_value(), "aged background can acquire");
        older_lease->release();
        auto newer = interactive.poll();
        require(newer.has_value(), "interactive resumes after aged waiter");
        newer->release();
        std::cout << "PASS FIFO, queue promotion, pressure protection and background aging\n" << std::flush;
        const auto concurrent_root = root / "concurrent";
        std::vector<std::future<ProcessOutput>> processes;
        for (int i = 0; i < 3; ++i)
            processes.push_back(std::async(std::launch::async, [&] {
                ProcessOptions options;
                options.timeout = Millis(15000);
                options.max_capture_chars = 8192;
                return spawn_process(path_text(executable_path()), {"--contend", path_text(concurrent_root)},
                                     options);
            }));
        for (auto& process : processes)
            process.get();
        SchedulerConfig inspection_config = config;
        inspection_config.root = concurrent_root;
        const auto snapshot = ExecutionScheduler(inspection_config).snapshot();
        require(snapshot["occupied"] == 0 && snapshot["global_queued"] == 0,
                "cross-process lease and ticket cleanup");
        const auto metrics = scheduler.snapshot()["local_process"];
        require(metrics["timed_out"] == 1 && metrics["cancelled"] >= 1 && metrics["active"] == 0,
                "scheduler metrics");
        std::cout << "PASS cross-process weighted contention and scheduler metrics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i)
        arguments.push_back(narrow(wide_argv[i]));
    std::vector<char*> argv;
    for (auto& value : arguments)
        argv.push_back(value.data());
    return run(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
