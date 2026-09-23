#include "devbox/scheduler.hpp"
#include "devbox/state_store.hpp"
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
        {
            auto index = open_state_store(root / "deadline-state");
            SchedulerConfig delayed;
            delayed.root = root / "delayed-index";
            delayed.lease_index = [index] {
                std::this_thread::sleep_for(Millis(20));
                return index;
            };
            ExecutionScheduler scheduler(delayed);
            auto request = background("metadata deadline");
            request.queue_timeout = Millis(5);
            rejects([&] { scheduler.acquire(request); }, "remained saturated");
            require(!fs::exists(delayed.root / "slot-00.json"),
                    "expired index admission releases its file claim");
            auto cancel = std::make_shared<Cancellation>();
            delayed.lease_index = [index, cancel] {
                cancel->cancel();
                return index;
            };
            ExecutionScheduler cancelled(delayed);
            request.queue_timeout = Millis(500);
            rejects([&] { cancelled.acquire(request, cancel); }, "queue wait cancelled");
            require(!fs::exists(delayed.root / "slot-00.json"),
                    "cancel during metadata admission cannot dispatch work");
        }
        {
            auto index = open_state_store(root / "state");
            SchedulerConfig indexed;
            indexed.root = root / "indexed-leases";
            indexed.lease_index = [index] { return index; };
            ExecutionScheduler scheduler(indexed);
            auto lease = scheduler.acquire(background("private label is not indexed"));
            auto records = index->list(StateQuery{"resource_lease"}).records;
            require(records.size() == 1 && records[0].status == "active" &&
                        !records[0].data.contains("label"),
                    "lease admission indexed before return");
            const auto first = records[0];
            lease.release();
            require(index->get("resource_lease", first.id)->status == "released",
                    "observed file release indexed");
            auto next = scheduler.acquire(background("replacement"));
            require(index->count("resource_lease") == 1 &&
                        index->get("resource_lease", first.id)->data["token"] != first.data["token"],
                    "fixed slot records remain bounded across reuse");
            lease.release();
            require(index->get("resource_lease", first.id)->status == "active",
                    "stale release cannot change a newer lease");
            next.release();
            auto lost = *index->get("resource_lease", first.id);
            const auto revision = lost.revision;
            lost.status = "active";
            StateMutation crash{lost, revision};
            index->apply({&crash, 1});
            require(scheduler.snapshot()["lease_index"]["reconciled_records"] == 1 &&
                        index->get("resource_lease", first.id)->status == "owner_lost_effects_unverified",
                    "crash reconciliation does not invent effect completion or replay work");
            auto releasing = scheduler.acquire(background("release acknowledgement loss"));
            index->release_writer();
            releasing.release();
            require(scheduler.snapshot()["local_process"]["lease_index_release_failures"] == 1 &&
                        scheduler.snapshot()["lease_index"]["reconciliation_pending"] == true,
                    "observed file release preserves result and exposes missing index acknowledgement");
            {
                auto foreground = scheduler.acquire({});
                require(!foreground.slots.empty(),
                        "ordinary foreground file index does not need a database transaction");
            }
            AcquireRequest resource;
            resource.resources.memory_bytes = 1;
            rejects([&] { scheduler.acquire(resource); }, "STATE");
            rejects([&] { scheduler.acquire(background("fenced writer")); }, "STATE");
            require(!fs::exists(indexed.root / "slot-00.json"),
                    "index admission failure releases file reservation before any work");
        }
        {
            SchedulerConfig bounded;
            // Exercise queue replacement with a final path below MAX_PATH but
            // too long to append another UUID to its filename (the CI failure).
            const auto root_length = path_text(root).size();
            bounded.root = root / std::string(root_length < 136 ? 136 - root_length : 16, 'r');
            bounded.max_concurrent = 3;
            bounded.reserved_interactive = 0;
            bounded.capacity = {100, 50, 80};
            bounded.heavy_capacity = 3;
            ExecutionScheduler resources(bounded);
            AcquireRequest first{ExecutionKind::interactive, ResourceClass::heavy, 2, "reserved", {}};
            first.resources = {60, 30, 40};
            auto held = resources.acquire(first);
            AcquireRequest second{ExecutionKind::interactive, ResourceClass::light, 1, "second", {}};
            second.resources = {20, 10, 10};
            auto other = resources.acquire(second);
            require(held.slots.size() == 2 && other.slots.size() == 1,
                    "weighted resource charge applied once per lease");
            auto check_blocked = [&](ResourceVector requested) {
                AcquireRequest probe{ExecutionKind::interactive, ResourceClass::watch, 1, "vector-probe", {}};
                probe.resources = requested;
                auto waiter = resources.begin(probe);
                require(!waiter.poll(), "memory/GPU/disk capacity shared across execution and watch pools");
            };
            check_blocked({30, 0, 0});
            check_blocked({0, 20, 0});
            check_blocked({0, 0, 40});
            other.release();
            AcquireRequest watch{ExecutionKind::interactive, ResourceClass::watch, 1, "released-memory", {}};
            watch.resources = {30, 0, 0};
            auto available = resources.acquire(watch);
            require(available.json()["resourceReservation"]["memory_bytes"] == 30,
                    "release returns exactly the declared vector capacity");
            watch.resources = {101, 0, 0};
            rejects([&] { (void)resources.begin(watch); }, "EXCEEDS_CAPACITY");
        }
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
        std::cout << "CHECK FIFO ordering\n" << std::flush;
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
        std::cout << "CHECK pressure protection\n" << std::flush;
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
        std::cout << "CHECK background aging\n" << std::flush;
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
