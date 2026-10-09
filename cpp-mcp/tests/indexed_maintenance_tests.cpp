#include "devbox/filesystem_worker.hpp"
#include "devbox/jobs.hpp"
#include <cstdlib>
#include <future>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
void batch_bounds(const fs::path& root) {
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->jobs_root = root / "jobs";
    config->state_root = root / "state";
    config->state_backend = "sqlite";
    config->job_retention_hours = 0;
    config->job_store_max_terminal_jobs = 0;
    config->job_store_max_bytes = 0;
    ScopeExit clean([&] { (void)stop_state_coordinator(config->state_root); });
    JobStore jobs(config);
    for (const auto* id : {"job-bounds-10000", "job-bounds-10001"}) {
        const auto paths =
            jobs.create_job(id, Json{{"id", id}, {"agent", {{"taskId", "bounded-batch"}}}},
                            Json{{"id", id}, {"status", "succeeded"}, {"completedAtUtc", utc_now()}});
        write_file(paths.stdout_log, std::string(1000, 'b'));
    }
    require(json_uint(jobs.reconcile_maintenance(2), "errors") == 0,
            "bounded batch fixture establishes complete sampled accounting");
    const auto store = jobs.index();
    {
        // Shift the first usage page so it cannot cover both job IDs. Existing
        // charges outside that page must be loaded individually, not replaced
        // with a default charge or a neighbouring row.
        const StateMutation extra{
            {"job_usage", "job-bounds-09999", "operator", "", "unknown", 0, Json{{"bytes", 0}}}, 0};
        store->apply({&extra, 1});
        const auto second = *store->get("job_usage", "job-bounds-10001");
        const auto result = jobs.reconcile_maintenance(2);
        require(json_uint(result, "errors") == 0 && json_uint(result, "scanned") == 2 &&
                    json_uint(result, "terminalRetained") == 2 &&
                    store->get("job_usage", second.id)->revision == second.revision + 1,
                "shifted usage pages retain per-ID fallback for unlisted existing charges");
    }
    const auto receipt = config->jobs_root / ".read-batch-worker.json";
    const auto dead = [&] {
        const auto owner = read_json(receipt);
        require(!process_matches_instance(json_uint(owner, "pid"), json_uint(owner, "instance")),
                "stalled batch worker is dead after timeout or cancellation acknowledgement");
    };
    const auto before = store->get("maintenance", "job-usage")->data;
    write_file(config->jobs_root / ".stall-read-batch", "owned fixture");
    const auto started = Clock::now();
    const auto result = jobs.reconcile_maintenance(2);
    require(Clock::now() - started < Millis(15000) && json_uint(result, "errors") == 0 &&
                json_uint(result, "storeBytes") == json_uint(before, "bytes") &&
                json_uint(result, "terminalRetained") == 2,
            "timed-out read batch recovers through bounded per-job inspections with exact accounting");
    dead();
    fs::remove(receipt);
    const auto before_cancel = *store->get("maintenance", "job-usage");
    const auto first_usage = *store->get("job_usage", "job-bounds-10000");
    const auto second_usage = *store->get("job_usage", "job-bounds-10001");
    auto cancel = std::make_shared<Cancellation>();
    auto work = std::async(std::launch::async, [&] {
        try {
            (void)jobs.reconcile_maintenance(2, cancel);
        } catch (const Cancelled&) {
            return true;
        }
        return false;
    });
    const auto ready_deadline = Clock::now() + Millis(3000);
    while (!fs::exists(receipt) && Clock::now() < ready_deadline)
        std::this_thread::sleep_for(Millis(5));
    const auto cancelled_at = Clock::now();
    cancel->cancel();
    require(work.get() && Clock::now() - cancelled_at < Millis(3000),
            "cancelling a live batch propagates without starting fallback inspections");
    dead();
    const auto after_cancel = *store->get("maintenance", "job-usage");
    require(after_cancel.revision == before_cancel.revision && after_cancel.data == before_cancel.data &&
                store->get("job_usage", first_usage.id)->revision == first_usage.revision &&
                store->get("job_usage", second_usage.id)->revision == second_usage.revision,
            "batch cancellation preserves the durable cursor and all sampled charges");
    require(read_file(jobs.paths("job-bounds-10000").stdout_log) == std::string(1000, 'b') &&
                read_file(jobs.paths("job-bounds-10001").stdout_log) == std::string(1000, 'b'),
            "batch timeout and cancellation leave retained artifacts byte-identical");
    fs::remove(config->jobs_root / ".stall-read-batch");
    fs::remove(receipt);
    const auto hold = config->jobs_root / ".hold-read-batch";
    write_file(hold, "owned fixture");
    ScopeExit release([&] {
        std::error_code error;
        fs::remove(hold, error);
    });
    const auto before_race = *store->get("maintenance", "job-usage");
    auto raced_usage = *store->get("job_usage", "job-bounds-10000");
    auto raced = std::async(std::launch::async, [&] {
        try {
            (void)jobs.reconcile_maintenance(2);
        } catch (const Error& error) {
            return std::string_view(error.what()) == "STATE_REVISION_CONFLICT";
        }
        return false;
    });
    const auto admitted_deadline = Clock::now() + Millis(3000);
    while (!fs::exists(receipt) && Clock::now() < admitted_deadline)
        std::this_thread::sleep_for(Millis(5));
    require(fs::exists(receipt), "batch worker admitted after usage-page prefetch");
    raced_usage.data["checked_at"] = "independent-fixture-update";
    const StateMutation concurrent{raced_usage, raced_usage.revision};
    store->apply({&concurrent, 1});
    fs::remove(hold);
    require(raced.get(), "prefetched charges remain protected by the transaction revision comparison");
    const auto after_race = *store->get("maintenance", "job-usage");
    require(after_race.revision == before_race.revision && after_race.data == before_race.data &&
                store->get("job_usage", raced_usage.id)->data == raced_usage.data,
            "a stale usage page cannot overwrite a concurrent charge or advance the durable cursor");
    require(json_uint(jobs.reconcile_maintenance(2), "errors") == 0,
            "a fresh page recovers after the concurrent-revision conflict");
    require(stop_state_coordinator(config->state_root), "bounded batch coordinator shutdown acknowledged");
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker([](std::string_view op, const Json& args) {
            if (op == "job_files_read_batch") {
                const auto root = path_from_utf8(json_string(args, "root"));
                if (fs::exists(root / ".fail-read-batch")) {
                    write_file(root / ".read-batch-failure-observed", "owned fixture");
                    throw Error("INJECTED_READ_BATCH_FAILURE");
                }
                if (fs::exists(root / ".stall-read-batch")) {
                    const auto pid = process_id();
                    write_json_atomic(root / ".read-batch-worker.json",
                                      Json{{"pid", pid}, {"instance", *process_instance(pid)}});
                    for (;;)
                        std::this_thread::sleep_for(Millis(1000));
                }
                if (fs::exists(root / ".hold-read-batch")) {
                    const auto pid = process_id();
                    write_json_atomic(root / ".read-batch-worker.json",
                                      Json{{"pid", pid}, {"instance", *process_instance(pid)}});
                    while (fs::exists(root / ".hold-read-batch"))
                        std::this_thread::sleep_for(Millis(5));
                }
            }
            if (op == "job_files" && json_bool(args, "remove") &&
                json_string(args, "id") == "job-test-10000") {
                const auto root = path_from_utf8(json_string(args, "root"));
                if (!fs::exists(root / ".crash-injected")) {
                    write_file(root / ".crash-injected", "owned fixture");
                    fs::remove(root / "job-test-10000" / "status.json");
                    std::_Exit(73);
                }
            }
            return filesystem_operation(op, args);
        });
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
        return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-maintenance-" + uuid());
    fs::create_directory(root);
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->jobs_root = root / "jobs";
    config->state_root = root / "state";
    config->state_backend = "sqlite";
    config->job_retention_hours = 1;
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
            const Json status{{"id", id},
                              {"status", "succeeded"},
                              {"completedAtUtc", i == 1 ? "2000-01-01T00:00:00Z" : utc_now()}};
            const auto paths = initial.create_job(id, request, status);
            write_file(paths.stdout_log, std::string(1000, 'x'));
            if (i <= 1) {
                ensure_directory(paths.dir / "research" / "documents");
                write_file(paths.dir / "research" / "ledger.json", "{}");
                write_file(paths.dir / "research" / "candidates-0.json", std::string(12000, 'a'));
                write_file(paths.dir / "research" / "candidates-1.json", std::string(13000, 'b'));
                {
                    FileLock ledger_lock(paths.dir / "research" / ".ledger.lock", Millis(1000), {}, true);
                }
                write_file(paths.dir / "research" / "documents" / "s1.json", std::string(30000, 'x'));
                const Json inspect{{"root", path_text(config->jobs_root)}, {"id", id}};
                for (const auto& unexpected : {"candidates-2.json", "candidates-00.json", "other.json"}) {
                    const auto file = paths.dir / "research" / unexpected;
                    write_file(file, "must remain");
                    bool rejected = false;
                    try {
                        auto removal = inspect;
                        removal["remove"] = true;
                        (void)job_filesystem_operation(removal);
                    } catch (const Error& error) {
                        rejected = std::string(error.what()).find("JOB_DIRECTORY_UNEXPECTED_ENTRY") !=
                                   std::string::npos;
                    }
                    require(rejected && fs::exists(file), "unknown research files deny all reclamation");
                    fs::remove(file);
                }
                const auto saved_status = read_json(paths.status);
                auto active_status = saved_status;
                active_status["status"] = "running";
                write_json_atomic(paths.status, active_status);
                auto removal = inspect;
                removal["remove"] = true;
                require(!json_bool(job_filesystem_operation(removal), "deleted") &&
                            fs::exists(paths.dir / "research" / "candidates-1.json"),
                        "recognized checkpoint files do not authorize active-job deletion");
                write_json_atomic(paths.status, saved_status);
            }
        }
        initial.write_operation_receipt("job-test-10000", Json{{"fingerprint", "retained-effect"},
                                                               {"submitted", true},
                                                               {"agent", {{"taskId", "fixture"}}}});
        {
            const auto first = initial.paths("job-test-10002"), second = initial.paths("job-test-10003");
            const auto old_status = read_file(first.status), old_log = read_file(first.stdout_log);
            const Json request{{"root", path_text(config->jobs_root)},
                               {"ids", Json::array({"job-test-10002", "job-test-10003"})}};
            auto batch = filesystem_operation("job_files_read_batch", request);
            require(batch.size() == 2 && json_bool(batch[0]["observation"], "exists") &&
                        json_uint(batch[0]["observation"], "bytes") >= 1000 &&
                        read_file(first.status) == old_status && read_file(first.stdout_log) == old_log,
                    "read batch observes exact retained bytes without mutating status or logs");
            for (const auto* field : {"remove", "compact", "prior_prune_intent"}) {
                auto mutating = request;
                mutating[field] = true;
                bool rejected = false;
                try {
                    (void)filesystem_operation("job_files_read_batch", mutating);
                } catch (const Error&) {
                    rejected = true;
                }
                require(rejected && read_file(first.stdout_log) == old_log,
                        "read batch refuses every mutation control");
            }
            for (const auto& ids : {Json::array(), Json::array({"job-test-10002", "job-test-10002"}),
                                    Json(std::vector<std::string>(65, "job-test-10002"))}) {
                auto invalid = request;
                invalid["ids"] = ids;
                bool rejected = false;
                try {
                    (void)filesystem_operation("job_files_read_batch", invalid);
                } catch (const Error&) {
                    rejected = true;
                }
                require(rejected, "read batch enforces nonempty, unique, bounded identifiers");
            }
            fs::create_directory(first.dir / "unexpected-batch-entry");
            batch = filesystem_operation("job_files_read_batch", request);
            require(batch[0].contains("error") && batch[1].contains("observation") &&
                        fs::exists(first.dir / "unexpected-batch-entry") && fs::exists(second.dir),
                    "one invalid directory does not hide other observations or authorize cleanup");
            fs::remove(first.dir / "unexpected-batch-entry");
        }
        // This unindexed directory would be invalid if maintenance still discovered arbitrary filesystem
        // jobs.
        fs::create_directory(config->jobs_root / "job-unindexed-poison");
        write_file(config->jobs_root / "job-unindexed-poison" / "status.json", "not-json");
        const auto first = initial.reconcile_maintenance(7);
        require(first["scanned"] == 7 && first["errors"] == 0 && first["batchLimited"] == true,
                "indexed maintenance visits exactly a bounded page, not unrelated directories");
        require(first["deleted"] == 1 && !fs::exists(initial.paths("job-test-10001").dir) &&
                    initial.get_status("job-test-10001")["artifactsAvailable"] == false,
                "expired research snapshots are reclaimed while terminal identity remains readable");
        const auto store = initial.index();
        const auto cursor = json_string(store->get("maintenance", "job-usage")->data, "after");
        require(!cursor.empty() && store->count("job_usage") == 7,
                "page cursor and per-job charges committed together");
        JobStore restarted(config);
        require(restarted.reconcile_maintenance(5)["scanned"] == 5 && store->count("job_usage") == 12 &&
                    json_string(store->get("maintenance", "job-usage")->data, "after") > cursor,
                "new frontend resumes the durable maintenance cursor without a directory index rebuild");
        require(json_uint(store->get("job_usage", "job-test-10000")->data, "bytes") > 55000,
                "research ledger, both candidate snapshots and document bytes are charged");
        Json result;
        std::uint64_t injected_errors = 0;
        bool pending_observed = false;
        for (int i = 0; i < 12; ++i) {
            result = restarted.enforce_store_quota();
            require(json_uint(result, "scanned") <= 64, "quota work bounded per call");
            injected_errors += json_uint(result, "errors");
            const auto first_job = store->get("job", "job-test-10000");
            pending_observed |= json_string(first_job->data, "artifacts_prune_state") == "pending";
            if (!json_bool(result, "quotaCyclePending") && !json_bool(result, "quotaPressure") &&
                json_bool(first_job->data, "artifacts_pruned"))
                break;
        }
        require(
            injected_errors == 1 && pending_observed,
            "actual worker crash after partial pruning retains a durable intent and reconciles next cycle");
        require(!json_bool(result, "quotaPressure") && json_uint(result, "terminalRetained") <= 4,
                "incremental quota reclamation converges");
        require(store->count("job") == 140 && initial.operation_receipt("job-test-10000").has_value(),
                "pruning logs retains admitted job and operation identity");
        require(initial.get_status("job-test-10000")["artifactsAvailable"] == false,
                "pruned terminal metadata remains readable and explicitly lacks artifacts");
        require(fs::exists(config->jobs_root / "job-unindexed-poison"),
                "unindexed files are not adopted or deleted");
        {
            write_file(config->jobs_root / ".fail-read-batch", "owned fixture");
            const auto before = store->get("maintenance", "job-usage")->data;
            Json result;
            for (int i = 0; i < 4; ++i) {
                result = restarted.enforce_store_quota();
                require(json_uint(result, "errors") == 0,
                        "read-batch failure falls back to the original per-job inspection path");
                if (!json_bool(result, "quotaCyclePending"))
                    break;
            }
            require(fs::exists(config->jobs_root / ".read-batch-failure-observed") &&
                        json_uint(result, "storeBytes") == json_uint(before, "bytes") &&
                        json_uint(result, "terminalRetained") == json_uint(before, "terminal_retained"),
                    "failed batch fallback preserves exact sampled accounting");
            fs::remove(config->jobs_root / ".fail-read-batch");
        }
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
        batch_bounds(root / "batch-bounds");
        std::cout
            << "Indexed maintenance pages, durable accounting, pruning receipts and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
