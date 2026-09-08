#include "devbox/engine.hpp"
#include <future>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw Error(message);
}
Json headers() {
    return Json{{"accept", "application/json"},
                {"content-type", "application/json"},
                {"user-agent", "devbox-engine-test"}};
}
Json invoke(const std::string& base, std::string name, Json args = Json::object()) {
    auto value = Json::parse(http_request("POST", base,
                                          Json{{"jsonrpc", "2.0"},
                                               {"id", uuid()},
                                               {"method", "tools/call"},
                                               {"params", {{"name", name}, {"arguments", args}}}}
                                              .dump(),
                                          headers(), Millis(15000), 8 * 1024 * 1024)
                                 .body);
    require(value.contains("result"), value.dump());
    return value["result"];
}
Json data(const Json& result) {
    require(!json_bool(result, "isError"), result.dump());
    return result["structuredContent"].value("data", Json());
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--job-runner")
        return run_job_request(std::make_shared<Config>(Config::load()), path_from_utf8(argv[2]));
    if (argc >= 2 && std::string_view(argv[1]) == "--probe") {
        std::cout << "probe-out";
        std::cerr << "probe-err";
        return argc > 2 ? std::stoi(argv[2]) : 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-engine-" + uuid());
    fs::create_directories(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        auto config = std::make_shared<Config>();
        config->project_root = root;
        config->host = "127.0.0.1";
        config->port = 0;
        config->platform = Platform::detect();
        config->runtime_mode = RuntimeMode::host;
        config->host_exec_enabled = true;
        config->devbox_auto_start = false;
        config->host_default_workdir = root;
        config->host_workspace_path = root;
        config->devbox_workspace_path = root;
        config->jobs_root = root / "jobs";
        config->execution_slot_root = root / "slots";
        config->mcp_performance_state_path = root / "run" / "perf.json";
        config->job_heartbeat_ms = 50;
        config->job_orphan_stale_ms = 1000;
        config->host_search_backend = "rust";
#ifdef _WIN32
        config->host_shell = "cmd.exe";
        config->power_shell_exe = "powershell.exe";
#else
        config->host_shell = "/bin/sh";
#endif
        const auto program = path_text(executable_path());
        const auto old_path = environment("PATH");
        set_environment("PATH", path_text(executable_path().parent_path()) +
#ifdef _WIN32
                                    ";" +
#else
                                    ":" +
#endif
                                    old_path.value_or(""));
        ScopeExit restore_path([&] { set_environment("PATH", old_path); });
        config->devbox_program_allowlist = {normalize_program(program)};
        config->host_program_allowlist = config->devbox_program_allowlist;
        ToolContract contract(*config);
        require(contract.all().size() == 45, "embedded authoritative tool count");
        const auto schema = contract.tool("devbox_wait_for_file")["inputSchema"]["properties"];
        require(schema["timeout_seconds"]["maximum"] == 85 && schema["poll_ms"]["minimum"] == 50,
                "runtime-configured schema bounds");
        bool rejected = false;
        try {
            (void)contract.arguments(
                "devbox_task_put",
                Json{{"task_id", "UPPER"}, {"expected_revision", 0}, {"state", Json::object()}});
        } catch (const Error&) {
            rejected = true;
        }
        require(rejected, "schema rejects invalid durable identity before mutation");
        config->max_wait_seconds = 2;
        require(
            ToolContract(*config).tool("devbox_wait")["inputSchema"]["properties"]["seconds"]["maximum"] == 2,
            "constrained schema");
        config->max_wait_seconds = 85;
        {
            auto probe_config = std::make_shared<Config>(*config);
            probe_config->jobs_root = root / "blocked-jobs";
            probe_config->execution_slot_root = root / "probe-slots";
            write_file(probe_config->jobs_root, "not a directory");
            BackgroundTasks background;
            RuntimeExecutor runtime(probe_config);
            ExecutionScheduler scheduler(SchedulerConfig::from(*probe_config));
            JobStore jobs(probe_config);
            UsageTelemetry usage(*probe_config, background);
            PerformanceMonitor performance(*probe_config, background, build_snapshot);
            OperationalMonitor monitor(probe_config, background, scheduler, jobs, runtime, performance, usage,
                                       [] { return 0; });
            bool failed = false;
            try {
                monitor.probe_store();
            } catch (const Error&) {
                failed = true;
            }
            require(failed && monitor.store_health()["ok"] == false && !monitor.ready(45),
                    "failed storage probe rejects readiness");
            require(monitor.store_health()["jobsWritable"] == false &&
                        disk_pressure(128ULL * 1024 * 1024, 1024ULL * 1024 * 1024, true) == "critical" &&
                        !monitor.reject_disk_work(ResourceClass::heavy, true, "cmake --build").has_value(),
                    "storage probe failure and advisory read-only admission");
            fs::remove(probe_config->jobs_root);
            monitor.probe_store();
            require(monitor.store_health()["ok"] == true &&
                        !monitor.reject_disk_work(ResourceClass::heavy, false, "cmake --build").has_value(),
                    "storage health recovers only after a successful real probe");
            usage.stop();
        }
        auto engine = std::make_shared<Engine>(config);
        HttpServer server(config, engine);
        const auto base = "http://127.0.0.1:" + std::to_string(server.start());
        engine->attach(server);
        ScopeExit stop([&] {
            server.stop();
            engine->stop();
        });
        require(engine->list_tools("2025-11-25").size() == 45 &&
                    !json_bool(engine->parity_report(), "cutover_allowed"),
                "all implemented tools advertised; certification still required before cutover");
        const auto capabilities = data(invoke(base, "devbox_capabilities"));
        require(capabilities["implementation"] == "cpp" && capabilities["tools"].size() == 45 &&
                    capabilities["schema_sha256"].get<std::string>().size() == 64,
                "native capabilities match actual dispatch");
        require(build_snapshot()["binarySha256"] == sha256_file(executable_path()),
                "identity refers to the actual executing C++ binary");
        const auto file = root / "file.txt";
        data(
            invoke(base, "devbox_write_file", Json{{"path", path_text(file)}, {"content", "alpha\nbeta\n"}}));
        require(invoke(base, "devbox_read_file",
                       Json{{"path", path_text(file)}})["structuredContent"]["stdout"] == "alpha\nbeta\n",
                "text file MCP roundtrip");
        require(data(invoke(base, "devbox_read_large_file",
                            Json{{"path", path_text(file)},
                                 {"offset_bytes", 6},
                                 {"max_bytes", 4}}))["content_base64"] == base64_encode("beta"),
                "large byte range MCP roundtrip");
        const auto binary = root / "binary.bin";
        require(data(invoke(base, "devbox_write_large_file",
                            Json{{"path", path_text(binary)}, {"content_base64", "AAH/"}}))["verified"] ==
                    true,
                "binary write verified");
        const auto state = data(invoke(base, "devbox_file_state", Json{{"path", "file.txt"}}));
        const Json atomic_args{{"path", "file.txt"},
                               {"expected_file_sha256", state["sha256"]},
                               {"append", true},
                               {"expected_offset_bytes", state["bytes"]},
                               {"content_base64", base64_encode("gamma\n")}};
        require(data(invoke(base, "devbox_write_file_atomic", atomic_args))["replayed"] == false,
                "atomic first commit");
        require(data(invoke(base, "devbox_write_file_atomic", atomic_args))["replayed"] == true,
                "atomic response-loss replay");
        auto wrong = atomic_args;
        wrong["content_base64"] = base64_encode("wrong");
        require(json_bool(invoke(base, "devbox_write_file_atomic", wrong), "isError"),
                "atomic conflict rejects changed payload");
        auto saved = data(invoke(base, "devbox_task_put",
                                 Json{{"task_id", "engine_task"},
                                      {"expected_revision", 0},
                                      {"state", {{"file", path_text(file)}}}}));
        require(saved["record"]["revision"] == 1 &&
                    data(invoke(base, "devbox_task_get",
                                Json{{"task_id", "engine_task"}}))["record"]["revision"] == 1,
                "task checkpoint saved and recovered");
        require(data(invoke(base, "devbox_task_list"))["tasks"].size() == 1, "task discovery pagination");
        require(data(invoke(base, "devbox_wait_for_file",
                            Json{{"path", path_text(file)},
                                 {"min_bytes", 3},
                                 {"stable_ms", 60},
                                 {"poll_ms", 50}}))["conditionMet"] == true,
                "stable file condition");
        auto waiting = std::async(std::launch::async, [&] {
            return invoke(
                base, "devbox_wait_for_file",
                Json{{"path", path_text(root / "arrives")}, {"timeout_seconds", 2.0}, {"poll_ms", 50}});
        });
        std::this_thread::sleep_for(Millis(100));
        write_file(root / "arrives", "yes");
        require(data(waiting.get())["conditionMet"] == true, "filesystem wait notices actual creation");
        require(data(invoke(base, "devbox_wait_for_file",
                            Json{{"path", path_text(root / "absent")},
                                 {"timeout_seconds", 0.1}}))["timedOut"] == true,
                "filesystem wait returns timeout state");
        require(json_bool(invoke(base, "devbox_read_file", Json{{"path", path_text(root / "absent")}}),
                          "isError"),
                "missing text file error envelope");
        auto direct =
            invoke(base, "devbox_run_program", Json{{"program", program}, {"args", {"--probe", "0"}}});
        require(direct["structuredContent"]["stdout"] == "probe-out" &&
                    direct["structuredContent"]["stderr"] == "probe-err" &&
                    data(direct)["execution"]["slot"].is_number(),
                "native process executes through weighted scheduler: " + direct.dump());
        auto failure =
            invoke(base, "host_run_program", Json{{"program", program}, {"args", {"--probe", "7"}}});
        require(json_bool(failure, "isError") && failure["structuredContent"]["exitCode"] == 7 &&
                    failure["structuredContent"]["stdout"] == "probe-out",
                "process error preserves exit and output");
        require(
            invoke(base, "devbox_exec", Json{{"command", "echo shell-probe"}})["structuredContent"]["stdout"]
                    .get<std::string>()
                    .find("shell-probe") != std::string::npos,
            "native runtime shell dispatch");
        const auto job_args = Json{{"task_id", "engine_task"},
                                   {"operation_id", "probe_once"},
                                   {"program", program},
                                   {"args", {"--probe", "0"}}};
        const auto job = data(invoke(base, "devbox_job_submit", job_args));
        const auto job_id = json_string(job, "id");
        require(!job_id.empty(), "durable job ID");
        require(data(invoke(base, "devbox_job_submit", job_args))["id"] == job_id, "durable job MCP replay");
        const auto terminal =
            data(invoke(base, "devbox_job_status", Json{{"job_id", job_id}, {"wait_seconds", 10}}));
        require(terminal["status"] == "succeeded",
                "native detached runner completes over MCP: " + terminal.dump());
        require(data(invoke(base, "devbox_job_logs", Json{{"job_id", job_id}}))["stdout"] == "probe-out",
                "durable logs roundtrip");
        require(data(invoke(base, "devbox_job_list", Json{{"task_id", "engine_task"}}))["jobs"].size() == 1,
                "durable job discovery");
        const auto search = invoke(base, "devbox_search_files",
                                   Json{{"path", path_text(root)}, {"pattern", "gamma"}, {"glob", "*.txt"}});
        require(search["structuredContent"]["stdout"].get<std::string>().find("gamma") != std::string::npos,
                "native search dispatch");
        const auto listing =
            invoke(base, "devbox_list_files", Json{{"path", path_text(root)}, {"max_entries", 10}});
        require(listing["structuredContent"]["stdout"].get<std::string>().find("file.txt") !=
                    std::string::npos,
                "native directory listing dispatch");
        const auto host = data(invoke(base, "host_status"));
        require(host == data(invoke(base, "windows_host_status")), "host status aliases");
        const auto inspect = data(invoke(base, "windows_host_inspect_file", Json{{"path", path_text(file)}}));
#ifdef _WIN32
        require(inspect["resolved_path"] == path_text(file) && inspect["utf8_valid"] == true,
                "file inspection dispatch");
#else
        require(inspect["resolved_path"] == replace_all(path_text(file), "/", "\\") &&
                    inspect["exists"] == false,
                "legacy Windows file tools retain Windows paths on POSIX");
#endif
        const auto stopped = data(invoke(base, "devbox_stop"));
        require(stopped.contains("controlMessage") && process_alive(process_id()),
                "host lifecycle stop preserves serving process");
        auto status = data(invoke(base, "devbox_status"));
        require(status["executionStore"]["ok"] == true &&
                    status["performance"]["process"]["pid"] == process_id(),
                "live status integrates actual operational health");
        require(json_uint(status["backgroundTasks"]["job-quota"], "lastSuccessUnixMs") > 0 &&
                    !status["backgroundTasks"].contains("job-quota-initial"),
                "initial quota enforcement is recorded on the supervised quota task");
        const auto before = Clock::now();
        std::vector<std::future<Json>> waits;
        for (int i = 0; i < 20; ++i)
            waits.push_back(std::async(std::launch::async,
                                       [&] { return invoke(base, "devbox_wait", Json{{"seconds", 0.3}}); }));
        require(http_request("GET", base + "/healthz").status == 200,
                "health during concurrent passive waits");
        for (auto& wait : waits)
            data(wait.get());
        require(Clock::now() - before < Millis(1800), "passive waits overlap outside native worker pools");
        server.stop();
        engine->stop();
        stop.disarm();
        require(read_file(root / "run" / "tool-usage.jsonl").find("tool_finish") != std::string::npos,
                "dispatcher usage records drained");
        std::cout << "Real C++ MCP dispatcher, files, jobs, shell, search, lifecycle, telemetry and passive "
                     "waits passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
