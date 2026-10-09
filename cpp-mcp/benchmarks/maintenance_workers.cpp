#include "devbox/allocator.hpp"
#include "devbox/filesystem_worker.hpp"
#include "devbox/jobs.hpp"
#include <iostream>

using namespace devbox;
namespace {
int run(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker();
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
        return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
    if (argc != 2)
        throw Error("Supply a new absolute fixture directory");
    const auto root = path_from_utf8(argv[1]);
    if (!root.is_absolute() || !fs::create_directory(root))
        throw Error("Fixture path must be new and absolute");
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->jobs_root = root / "jobs";
    config->state_root = root / "state";
    config->state_backend = "sqlite";
    config->job_retention_hours = 0;
    config->job_store_max_terminal_jobs = 10000;
    config->job_store_max_bytes = 1024 * 1024 * 1024;
    ensure_directory(config->jobs_root);
    ScopeExit clean([&] { (void)stop_state_coordinator(config->state_root); });
    JobStore jobs(config);
    const auto store = jobs.index();
    constexpr std::size_t records = 512, retained = 64;
    std::uint64_t expected_bytes = 0;
    for (std::size_t i = 0; i < records; ++i) {
        const auto id = "job-profile-" + std::to_string(10000 + i);
        const Json status{{"id", id}, {"status", "succeeded"}, {"completedAtUtc", "2000-01-01T00:00:00Z"}};
        if (i % 8 == 0) {
            const Json request{{"id", id}, {"agent", {{"taskId", "fixture"}}}};
            const auto paths = jobs.create_job(id, request, status);
            write_file(paths.stdout_log, std::string(1024, 'x'));
            for (const auto& file : fs::directory_iterator(paths.dir))
                expected_bytes += file.file_size();
        } else {
            const StateMutation row{{"job", id, "operator", "fixture", "succeeded", 0,
                                     Json{{"status", status},
                                          {"agent", {{"taskId", "fixture"}}},
                                          {"artifacts_pruned", true},
                                          {"request_sha256", std::string(64, 'a')}}},
                                    0};
            store->apply({&row, 1});
        }
        const StateMutation usage{
            {"job_usage", id, "operator", "", "unknown", 0,
             Json{{"bytes", 0}, {"compacted", true}, {"checked_at", "2000-01-01T00:00:00Z"}}},
            0};
        store->apply({&usage, 1});
    }
    std::cout << Json{{"benchmark", "retained-job-maintenance-workers"},
                      {"records", records},
                      {"retained", retained},
                      {"expected_bytes", expected_bytes},
                      {"scope",
                       "Real isolated filesystem workers and encrypted state IPC; synthetic fixtures"}}
                     .dump()
              << '\n'
              << std::flush;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        std::size_t scanned = 0, pages = 0;
        const auto before = allocator_counters();
        const auto started = Clock::now();
        Json result;
        do {
            result = jobs.enforce_store_quota();
            if (json_uint(result, "errors") || ++pages > records / 64 + 1)
                throw Error("Maintenance fixture failed or exceeded its page bound");
            scanned += json_uint(result, "scanned");
        } while (json_bool(result, "quotaCyclePending"));
        const auto stopped = Clock::now();
        const auto after = allocator_counters();
        if (scanned != records || json_uint(result, "storeBytes") != expected_bytes ||
            json_uint(result, "terminalRetained") != retained || !json_bool(result, "accountingComplete") ||
            store->count("job") != records || store->count("job_usage", {}, "retained_terminal") != retained)
            throw Error("Maintenance state/accounting oracle failed");
        std::cout << Json{{"cycle", cycle},
                          {"warmup", cycle == 0},
                          {"scanned", scanned},
                          {"pages", pages},
                          {"state_oracle", true},
                          {"wall_ms", std::chrono::duration<double, std::milli>(stopped - started).count()},
                          {"parent_allocated_bytes", after.allocated - before.allocated},
                          {"parent_allocation_calls", after.calls - before.calls}}
                         .dump()
                  << '\n'
                  << std::flush;
    }
    if (!stop_state_coordinator(config->state_root))
        throw Error("Owned coordinator did not stop");
    clean.disarm();
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
