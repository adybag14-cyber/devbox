#include "devbox/admission.hpp"
#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include "devbox/monitoring.hpp"
#include "devbox/state_store.hpp"
#include <iostream>

using namespace devbox;
namespace {
template <class Fn> void measure(std::string_view name, std::size_t count, Fn operation) {
    std::uint64_t observed = 0;
    for (unsigned i = 0; i < 3; ++i)
        observed += static_cast<std::uint64_t>(operation());
    const auto before = allocator_counters();
    const auto started = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
        observed += static_cast<std::uint64_t>(operation());
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto after = allocator_counters();
    std::cout << Json{{"component", name},
                      {"iterations", count},
                      {"warmup", 3},
                      {"wall_ms", elapsed},
                      {"allocated_bytes", after.allocated - before.allocated},
                      {"allocation_calls", after.calls - before.calls},
                      {"retained_bytes_delta",
                       static_cast<std::int64_t>(after.current) - static_cast<std::int64_t>(before.current)},
                      {"observed", observed}}
                     .dump()
              << '\n'
              << std::flush;
}
int run(const fs::path& root) {
    if (!root.is_absolute() || !fs::create_directory(root))
        throw Error("Supply a new absolute fixture directory");
    Config config;
    config.project_root = root;
    config.platform = Platform::detect();
    config.runtime_mode = RuntimeMode::host;
    config.host_default_workdir = root;
    config.node_exe = "node";
    config.power_shell_exe = "powershell.exe";
    config.mcp_performance_state_path = root / "performance.json";
    const auto json_file = root / "small.json";
    write_json_atomic(json_file, Json{{"sampledAtUtc", "2026-10-22T12:53:00.000Z"}, {"ok", true}});
    const auto stored = read_json(json_file);
    const auto private_directory = root / "private";
    ensure_private_state_directory(private_directory);
    ensure_directory(root / "run");
    (void)operator_admission(config, "resume");
    AdmissionControl admission(config);
    BackgroundTasks background;
    for (unsigned i = 0; i < 14; ++i) {
        const auto name = "fixture-" + std::to_string(i);
        background.mark_started(name, "periodic");
        background.success(name);
    }
    PerformanceMonitor performance(config, background, build_snapshot);
    SchedulerConfig scheduler_config;
    scheduler_config.root = root / "slots";
    ExecutionScheduler scheduler(scheduler_config);
    std::cout << Json{{"benchmark", "idle-components"},
                      {"build", build_snapshot()},
                      {"scope", "Synthetic local fixture; no production state or services"}}
                     .dump()
              << '\n';
    measure("utc_format", 1000, [] { return utc_now().size(); });
    measure("utc_parse", 1000, [] { return parse_utc("2026-10-22T12:53:00.000Z").has_value(); });
    measure("snapshot_age", 1000, [&] { return snapshot_age(stored).has_value(); });
    measure("build_snapshot", 1000, [] { return build_snapshot().size(); });
    measure("small_json_read", 200, [&] { return read_json(json_file, 4096).size(); });
    measure("private_directory_check", 100, [&] {
        ensure_private_state_directory(private_directory);
        return 1;
    });
    measure("admission_refresh", 100, [&] {
        admission.refresh();
        return admission.snapshot().size();
    });
    measure("background_snapshot", 200, [&] { return background.snapshot().size(); });
    measure("scheduler_snapshot", 100, [&] { return scheduler.snapshot().size(); });
    measure("performance_persist_empty_samples", 20, [&] {
        performance.persist();
        return performance.snapshot().size();
    });
    measure("find_program_pwsh", 100, [] { return find_program("pwsh").has_value(); });
    RuntimeExecutor runtime(std::make_shared<Config>(config));
    measure("runtime_version_refresh", 3, [&] { return runtime.get_versions(true).size(); });
    const auto state_root = root / "state";
    StateClientOptions options;
    options.executable = executable_path();
    ScopeExit stop([&] { (void)stop_state_coordinator(state_root); });
    auto index = open_coordinated_state(state_root, options);
    scheduler_config.root = root / "indexed-slots";
    scheduler_config.lease_index = [index] { return index; };
    ExecutionScheduler indexed_scheduler(scheduler_config);
    measure("indexed_scheduler_snapshot", 100, [&] { return indexed_scheduler.snapshot().size(); });
    if (!stop_state_coordinator(state_root))
        throw Error("Owned fixture coordinator did not stop");
    stop.disarm();
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
            return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
        if (argc == 3 && std::string_view(argv[1]) == "--stop-fixture-coordinator")
            return stop_state_coordinator(path_from_utf8(argv[2])) ? 0 : 1;
        if (argc != 2)
            throw Error("Usage: devbox-idle-components-bench NEW_FIXTURE_DIRECTORY");
        return run(path_from_utf8(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
