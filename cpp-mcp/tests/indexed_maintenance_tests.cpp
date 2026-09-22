#include "devbox/filesystem_worker.hpp"
#include "devbox/jobs.hpp"
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker();
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
        return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-maintenance-" + uuid());
    fs::create_directory(root);
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->jobs_root = root / "jobs";
    config->state_root = root / "state";
    config->state_backend = "sqlite";
    config->job_retention_hours = 0;
    config->job_store_max_terminal_jobs = 4;
    config->job_store_max_bytes = 1000000;
    ScopeExit clean([&] {
        if (stop_state_coordinator(config->state_root)) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    });
    try {
        JobStore initial(config);
        for (int i = 0; i < 140; ++i) {
            const auto id = "job-test-" + std::to_string(10000 + i);
            const Json request{{"id", id}, {"agent", {{"taskId", "fixture"}}}};
            const Json status{{"id", id}, {"status", "succeeded"}, {"completedAtUtc", utc_now()}};
            const auto paths = initial.create_job(id, request, status);
            write_file(paths.stdout_log, std::string(1000, 'x'));
        }
        initial.write_operation_receipt("job-test-10000", Json{{"fingerprint", "retained-effect"},
                                                               {"submitted", true},
                                                               {"agent", {{"taskId", "fixture"}}}});
        // This unindexed directory would be invalid if maintenance still discovered arbitrary filesystem
        // jobs.
        fs::create_directory(config->jobs_root / "job-unindexed-poison");
        write_file(config->jobs_root / "job-unindexed-poison" / "status.json", "not-json");
        const auto first = initial.reconcile_maintenance(7);
        require(first["scanned"] == 7 && first["errors"] == 0 && first["batchLimited"] == true,
                "indexed maintenance visits exactly a bounded page, not unrelated directories");
        const auto store = initial.index();
        const auto cursor = json_string(store->get("maintenance", "job-usage")->data, "after");
        require(!cursor.empty() && store->count("job_usage") == 7,
                "page cursor and per-job charges committed together");
        JobStore restarted(config);
        require(restarted.reconcile_maintenance(5)["scanned"] == 5 && store->count("job_usage") == 12 &&
                    json_string(store->get("maintenance", "job-usage")->data, "after") > cursor,
                "new frontend resumes the durable maintenance cursor without a directory index rebuild");
        Json result;
        for (int i = 0; i < 12; ++i) {
            result = restarted.enforce_store_quota();
            require(json_uint(result, "scanned") <= 64 && result["errors"] == 0,
                    "quota work bounded per call");
            if (!json_bool(result, "quotaCyclePending") && !json_bool(result, "quotaPressure"))
                break;
        }
        require(!json_bool(result, "quotaPressure") && json_uint(result, "terminalRetained") <= 4,
                "incremental quota reclamation converges");
        require(store->count("job") == 140 && initial.operation_receipt("job-test-10000").has_value(),
                "pruning logs retains admitted job and operation identity");
        require(initial.get_status("job-test-10000")["artifactsAvailable"] == false,
                "pruned terminal metadata remains readable and explicitly lacks artifacts");
        require(fs::exists(config->jobs_root / "job-unindexed-poison"),
                "unindexed files are not adopted or deleted");
        const auto retained =
            store->list(StateQuery{"job_usage", "operator", {}, "retained_terminal", {}, 1}).records;
        require(!retained.empty(), "one retained fixture for accounting replay");
        const auto id = retained.front().id;
        fs::create_directory(initial.paths(id).dir / "unexpected-subdirectory");
        config->job_store_max_terminal_jobs = 1;
        bool saw_error = false;
        for (int i = 0; i < 4; ++i)
            saw_error |= json_uint(restarted.enforce_store_quota(), "errors") > 0;
        require(saw_error && fs::exists(initial.paths(id).dir / "unexpected-subdirectory"),
                "unexpected nested content is retained and reported, never recursively discarded");
        const auto meter = store->get("maintenance", "job-usage");
        std::uint64_t sum = 0, terminals = 0;
        std::optional<std::string> after;
        do {
            const auto page = store->list(StateQuery{"job_usage", "operator", {}, {}, after, 100});
            for (const auto& item : page.records) {
                sum += json_uint(item.data, "bytes");
                terminals += item.status == "retained_terminal";
            }
            after = page.next;
        } while (after);
        require(sum == json_uint(meter->data, "bytes") &&
                    terminals == json_uint(meter->data, "terminal_retained"),
                "replayed scans and failed pages preserve exact aggregate of sampled charges");
        auto cancel = std::make_shared<Cancellation>();
        cancel->cancel();
        bool cancelled = false;
        try {
            (void)restarted.enforce_store_quota(cancel);
        } catch (const Cancelled&) {
            cancelled = true;
        }
        require(cancelled, "maintenance honors cancellation before filesystem effects");
        std::cout
            << "Indexed maintenance pages, durable accounting, pruning receipts and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
