#include "devbox/jobs.hpp"
#include <iostream>
#include <thread>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
int run(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator") {
        try {
            return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
        } catch (...) {
            return 2;
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "--job-runner")
        return run_job_request(std::make_shared<Config>(Config::load(false)), path_from_utf8(argv[2]));
    if (argc == 4 && std::string_view(argv[1]) == "--counter") {
        if (std::string_view(argv[3]) == "hold") {
            const auto deadline = Clock::now() + Millis(5000);
            const auto release = path_from_utf8(std::string(argv[2]) + ".release");
            while (!fs::exists(release) && Clock::now() < deadline)
                std::this_thread::sleep_for(Millis(5));
            if (!fs::exists(release))
                return 2;
        } else
            std::this_thread::sleep_for(Millis(std::stoi(argv[3])));
        atomic_write(path_from_utf8(argv[2]), "x", true);
        return 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-indexed-jobs-" + uuid());
    ensure_directory(root);
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->jobs_root = root / "jobs";
    config->state_root = root / "run" / "state";
    config->execution_slot_root = root / "slots";
    config->host_workspace_path = config->host_default_workdir = config->devbox_workspace_path = root;
    config->platform = Platform::detect();
    config->host_exec_enabled = true;
    config->node_exe = path_text(executable_path());
    config->host_program_allowlist = config->devbox_program_allowlist = {"node"};
    config->job_heartbeat_ms = 50;
    config->job_orphan_stale_ms = 1000;
    ScopeExit cleanup([&] {
        if (stop_state_coordinator(config->state_root)) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    });
    try {
        JobManager legacy(config);
        ProgramRequest program;
        program.program = "node";
        program.args = {"--counter", path_text(root / "counter"), "hold"};
        program.working_dir = root;
        program.timeout = Millis(10000);
        Submission operation{"migration_task", "once", "fixture"};
        const auto submitted = legacy.submit_program(program, operation);
        const auto id = json_string(submitted, "id");
        bool refused = false;
        try {
            (void)migrate_legacy_state(config);
        } catch (const Error& error) {
            refused =
                std::string_view(error.what()).find("STATE_MIGRATION_ACTIVE_JOB") != std::string_view::npos;
        }
        require(refused, "migration cannot change authority while durable runners are active");
        write_file(root / "counter.release", "release");
        require(legacy.store().wait_status(id, Millis(5000), true, Millis(20))["status"] == "succeeded",
                "legacy fixture completes");
        for (std::uint64_t revision = 0; revision < 7; ++revision)
            task_put(root / "run" / "tasks", "checkpoint", revision, Json{{"counter", revision}});
        const auto before = task_get(root / "run" / "tasks", "checkpoint");
        const auto migrated = migrate_legacy_state(config);
        require(migrated["jobs"] == 1 && migrated["operations"] == 1 && migrated["tasks"] == 1,
                "migration imports jobs, receipts and revisioned checkpoints");
        config->state_backend = "sqlite";
        {
            auto obsolete = std::make_shared<Config>(*config);
            obsolete->state_backend = "legacy";
            JobManager fenced(obsolete);
            bool denied = false;
            try {
                (void)fenced.submit_program(program, operation);
            } catch (const Error& error) {
                denied = std::string_view(error.what()).starts_with("STATE_LEGACY_BACKEND_FENCED");
            }
            require(denied, "legacy writable mode cannot resurrect effects after SQLite cutover");
        }
        auto canonical_config = std::make_shared<Config>(*config);
        canonical_config->project_root = fs::canonical(config->project_root);
        canonical_config->jobs_root = fs::canonical(config->jobs_root);
        require(JobStore(canonical_config).index()->count("job") == 1,
                "canonical runner paths resolve the same migrated layout");
        JobManager indexed(config);
        require(indexed_task_get(indexed.store(), "checkpoint") == before,
                "checkpoint revision and byte hash survive migration");
        require(indexed.submit_program(program, operation)["replayed"] == true &&
                    read_file(root / "counter") == "x",
                "migrated operation receipts cannot resurrect an already executed effect");
        require(migrate_legacy_state(config)["replayed"] == true,
                "migration replay does not reimport stale file mirrors");
        operation.operation_id = "after_migration";
        const auto fresh = indexed.submit_program(program, operation);
        const auto next = json_string(fresh, "id");
        const auto terminal = indexed.store().wait_status(next, Millis(5000), true, Millis(20));
        if (terminal["status"] != "succeeded")
            std::cerr << "Indexed runner status: " << terminal.dump() << '\n';
        require(terminal["status"] == "succeeded" && read_file(root / "counter") == "xx",
                "new detached runners write authoritative indexed state");
        const auto page = indexed.store().list({}, {}, {}, 1);
        require(page["jobs"].size() == 1 && page["next_cursor"].is_string(),
                "indexed job page returns a bounded cursor");
        require(indexed.store().operation_count() == 2, "receipt admission uses the indexed count");
        auto checkpoint = indexed_task_put(indexed.store(), "checkpoint", 7, Json{{"counter", 7}});
        require(checkpoint["record"]["revision"] == 8 &&
                    indexed_task_list(indexed.store(), {}, 10)["tasks"][0]["revision"] == 8,
                "indexed checkpoints retain CAS and list summaries");
        auto request = indexed.store().read_request(next);
        const auto original = request;
        request["args"] = {"--counter", path_text(root / "counter"), "0"};
        write_json_atomic(indexed.store().paths(next).request, request);
        bool integrity = false;
        try {
            (void)indexed.store().read_request(next);
        } catch (const Error& error) {
            integrity = std::string_view(error.what()).starts_with("JOB_REQUEST_INTEGRITY");
        }
        require(integrity, "runner inputs are pinned to the admitted request content");
        write_json_atomic(indexed.store().paths(next).request, original);
        require(stop_state_coordinator(config->state_root), "owned indexed state service stopped");
        std::cout << "Legacy migration, durable receipts, indexed jobs, CAS checkpoints and request "
                     "integrity passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_args) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(narrow(wide_args[i]));
    std::vector<char*> values;
    for (auto& value : args)
        values.push_back(value.data());
    return run(argc, values.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
