#include "devbox/jobs.hpp"
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
int run(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--job-runner")
        return run_job_request(std::make_shared<Config>(Config::load()), path_from_utf8(argv[2]));
    if (argc >= 3 && std::string(argv[1]) == "--child") {
        const std::string mode = argv[2];
        if (mode == "count" && argc == 4) {
            atomic_write(path_from_utf8(argv[3]), "x", true);
            std::cout << "executed-once";
        }
        if (mode == "sleep") {
            std::cout << "started" << std::flush;
            std::this_thread::sleep_for(Millis(10000));
        }
        if (mode == "log") {
            std::cout << std::string(30000, 'o') << "OUT-END";
            std::cerr << std::string(18000, 'e') << "ERR-END";
        }
        if (mode == "delay-marker" && argc == 4) {
            std::this_thread::sleep_for(Millis(350));
            atomic_write(path_from_utf8(argv[3]), "survived-parent");
        }
        if (mode == "fail") {
            std::cerr << "planned failure";
            return 9;
        }
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--launch-and-exit") {
        const auto config = std::make_shared<Config>(Config::load());
        JobManager manager(config);
        ProgramRequest request;
        request.program = "node";
        request.args = {"--child", "delay-marker", argv[2]};
        request.timeout = Millis(3000);
        std::cout << manager.start_program(request).dump();
        return 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-jobs-" + uuid());
    fs::create_directory(root);
#ifndef _WIN32
    fs::permissions(root, fs::perms::owner_all);
#endif
    std::vector<std::pair<std::uint32_t, std::optional<std::uint64_t>>> owned;
    Environment original;
    const auto set = [&](const std::string& key, const std::string& value) {
        if (const auto prior = environment(key))
            original[key] = *prior;
        else
            original[key] = "__DEVBOX_TEST_MISSING__";
        set_environment(key, value);
    };
    ScopeExit cleanup([&] {
        for (const auto& [pid, instance] : owned)
            if (instance && process_matches_instance(pid, instance))
                terminate_process_tree(pid, instance);
        for (const auto& [key, value] : original)
            set_environment(key, value == "__DEVBOX_TEST_MISSING__" ? std::nullopt : std::optional(value));
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        set("DEVBOX_PROJECT_ROOT", path_text(root));
        set("DEVBOX_RUNTIME_MODE", "host");
        set("ENABLE_HOST_EXEC", "true");
        set("MCP_AUTH_MODE", "none");
        set("MCP_JOBS_ROOT", path_text(root / "jobs"));
        set("MCP_EXEC_SLOT_ROOT", path_text(root / "slots"));
        set("HOST_WORKSPACE_PATH", path_text(root));
        set("HOST_DEFAULT_WORKDIR", path_text(root));
        set("NODE_EXE", path_text(executable_path()));
        set("DEVBOX_PROGRAM_ALLOWLIST", "node");
        set("MCP_JOB_LOG_MAX_BYTES", "4096");
        set("MCP_JOB_LOG_ROTATIONS", "1");
        set("MCP_JOB_HEARTBEAT_MS", "1000");
        set("MCP_JOB_ORPHAN_STALE_MS", "1000");
        set("MCP_BACKGROUND_QUEUE_TIMEOUT_MS", "3000");
#ifdef _WIN32
        set("LOCALAPPDATA", path_text(root));
        set("HOST_SHELL", env_or("COMSPEC", "cmd.exe"));
#else
        set("HOME", path_text(root));
        set("HOST_SHELL", "/bin/sh");
#endif
        auto config = std::make_shared<Config>(Config::load());
        JobManager manager(config);
        auto& store = manager.store();
        const auto track = [&](const Json& value) {
            const auto& summary = value.contains("job") ? value["job"] : value;
            if (summary.contains("runnerPid") && summary["runnerPid"].is_number_unsigned()) {
                const auto pid = summary["runnerPid"].get<std::uint32_t>();
                owned.emplace_back(pid, process_instance(pid));
            }
        };
        const auto finish = [&](const std::string& id) {
            const auto value = store.wait_status(id, Millis(10000), true, Millis(50));
            require(terminal_status(json_string(value, "status")), "job reached terminal status");
            return value;
        };
        require(infer_program_resource("node", {"-e", "console.log('cargo build')"}) == ResourceClass::light,
                "program classification ignores payload");
        require(infer_program_resource("rg", {"-V"}) == ResourceClass::light &&
                    infer_program_resource("rg", {"hello"}) == ResourceClass::io_heavy,
                "ripgrep classification");
        require(infer_shell_resource("cargo build && sleep 2") == ResourceClass::heavy &&
                    infer_shell_resource("sleep 2") == ResourceClass::watch,
                "shell classification");
        ProgramRequest request;
        request.program = "node";
        request.args = {"--child", "count", path_text(root / "counter")};
        request.timeout = Millis(5000);
        Submission operation{"task-a", "operation-a", "count once"};
        auto first = manager.submit_program(request, operation);
        track(first);
        const auto id = first["id"].get<std::string>();
        require(first["replayed"] == false &&
                    id == "job-op-" + sha256(Json::array({"task-a", "operation-a"}).dump()),
                "deterministic operation identity");
        const auto completed = finish(id);
        require(completed["status"] == "succeeded" && completed["exitCode"] == 0 &&
                    completed.contains("logs"),
                "detached job success journal");
        require(manager.submit_program(request, operation)["replayed"] == true &&
                    read_file(root / "counter") == "x",
                "durable operation replay");
        request.args.back() = path_text(root / "different-counter");
        rejects([&] { manager.submit_program(request, operation); }, "OPERATION_CONFLICT");
        request.args.back() = path_text(root / "counter");
        const auto receipt = read_json(config->jobs_root / ".operations" / path_from_utf8(id + ".json"));
        auto identity = store.read_request(id);
        identity.erase("createdAtUtc");
        require(receipt["fingerprint"] == sha256(canonical_json(identity).dump()) &&
                    receipt["submitted"] == true,
                "persisted canonical fingerprint");
        require(store.list("task-a", {}, {}, 1)["jobs"][0]["id"] == id, "task-filtered job listing");
        auto more = manager.start_program(ProgramRequest{"node", {"--child", "log"}, {}, root, Millis(5000)});
        track(more);
        const auto log_id = more["id"].get<std::string>();
        require(finish(log_id)["status"] == "succeeded", "log job completion");
        const auto logs = store.logs(log_id, 5000);
        require(logs["stdout"].get<std::string>().ends_with("OUT-END") &&
                    logs["stderr"].get<std::string>().ends_with("ERR-END"),
                "rotated log tails");
        require(logs["logs"]["truncated"] == true &&
                    logs["logs"]["stdout"]["totalBytes"].get<std::uint64_t>() <= 8192,
                "rotated log disk bound");
        more = manager.start_program(ProgramRequest{"node", {"--child", "fail"}, {}, root, Millis(5000)});
        track(more);
        auto failed = finish(more["id"].get<std::string>());
        require(failed["status"] == "failed" && failed["exitCode"] == 9, "failed job journal");
        more = manager.start_program(ProgramRequest{"node", {"--child", "sleep"}, {}, root, Millis(1000)});
        track(more);
        const auto timeout_result = finish(more["id"].get<std::string>());
        if (timeout_result["status"] != "timed_out")
            throw Error("job timeout distinct from cancellation: " + timeout_result.dump());
        more = manager.start_program(ProgramRequest{"node", {"--child", "sleep"}, {}, root, Millis(10000)});
        track(more);
        const auto cancelled_id = more["id"].get<std::string>();
        const auto deadline = Clock::now() + Millis(3000);
        while (json_string(store.get_status(cancelled_id), "status") != "running" && Clock::now() < deadline)
            std::this_thread::sleep_for(Millis(10));
        const auto cancelled = store.cancel(cancelled_id);
        require(cancelled["status"] == "cancelled" && cancelled["runnerAlive"] == false &&
                    !cancelled["completedAtUtc"].is_null(),
                "verified cancellation acknowledgement");
        std::cout << "PASS detached execution, receipts, conflicts, logs, failure, timeout and cancellation\n"
                  << std::flush;
        // Restarting a runner on a completed request cannot repeat side effects.
        run_job_request(config, store.paths(id).request);
        require(read_file(root / "counter") == "x", "completed runner is never resurrected");
        const auto launch = spawn_process(path_text(executable_path()),
                                          {"--launch-and-exit", path_text(root / "parent-marker")});
        const auto surviving = Json::parse(launch.stdout_text);
        track(surviving);
        require(finish(surviving["id"].get<std::string>())["status"] == "succeeded" &&
                    read_file(root / "parent-marker") == "survived-parent",
                "detached runner survives submitting process exit");
        // Preserve operation receipts when ordinary result retention removes a job directory.
        for (const auto& [pid, instance] : owned) {
            const auto until = Clock::now() + Millis(1000);
            while (process_matches_instance(pid, instance) && Clock::now() < until)
                std::this_thread::sleep_for(Millis(10));
        }
        fs::remove_all(store.paths(id).dir);
        require(manager.submit_program(request, operation)["job"]["status"] == "result_expired" &&
                    read_file(root / "counter") == "x",
                "expired result does not reexecute");
        const std::string orphan_id = "job-orphan-fixture";
        store.create_job(
            orphan_id, Json::object(),
            Json{{"id", orphan_id},
                 {"status", "running"},
                 {"createdAtUtc", "2000-01-01T00:00:00Z"},
                 {"runnerPid", process_id()},
                 {"runnerProcessInstance", std::to_string(*process_instance(process_id()) + 1)}});
        require(store.get_status(orphan_id)["status"] == "interrupted",
                "orphan reconciliation with mismatched identity");
        const std::string pending_id = "job-pending-fixture";
        store.create_job(pending_id, Json::object(),
                         Json{{"id", pending_id},
                              {"status", "running"},
                              {"createdAtUtc", utc_now()},
                              {"runnerPid", process_id()},
                              {"runnerProcessInstance", std::to_string(*process_instance(process_id()))}});
        atomic_write(store.paths(pending_id).cancel, "cancel");
        const auto pending = store.get_status(pending_id);
        require(pending["status"] == "cancel_requested" && pending["completedAtUtc"].is_null(),
                "live owner remains cancellation pending");
        const auto maintenance = store.reconcile_maintenance(2);
        require(maintenance["scanned"].get<std::size_t>() <= 2, "bounded maintenance batch");
        config->job_store_max_terminal_jobs = 2;
        const auto quota = store.enforce_store_quota();
        require(quota["terminalRetained"].get<std::size_t>() <= 2 && fs::exists(store.paths(pending_id).dir),
                "quota retains active jobs");
        require(fs::exists(config->jobs_root / ".operations" / path_from_utf8(id + ".json")),
                "quota retains operation receipts");
        std::cout
            << "PASS parent exit, terminal replay protection, orphan states, bounded maintenance and quota\n";
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
    try {
        return run(argc, argv.data());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
