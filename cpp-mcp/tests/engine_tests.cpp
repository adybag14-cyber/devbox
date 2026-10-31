#include "devbox/chrome_bridge.hpp"
#include "devbox/engine.hpp"
#include "devbox/filesystem_worker.hpp"
#include <future>
#include <iostream>
#include <limits>
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
    if (argc == 2 && std::string_view(argv[1]) == "--request") {
        const auto request = Json::parse(std::cin);
        if (json_string(request, "action") == "click") {
            std::cout
                << Json{{"ok", false}, {"input_sent", false}, {"error", {{"code", "FIXTURE_DENIED"}}}}.dump();
            return 2;
        }
        std::cout << Json{{"ok", true},
                          {"data", {{"fixture", "chrome-bridge"}, {"action", request.at("action")}}}}
                         .dump();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker();
    if (argc == 3 && std::string_view(argv[1]) == "--job-runner")
        return run_job_request(std::make_shared<Config>(Config::load(false)), path_from_utf8(argv[2]));
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
        {
            const Json snapshot{{"occupied", 7}, {"global_queued", 3}};
            const std::string sampled_at = "1970-01-01T00:00:00.000Z";
            Json cache{{"ok", true}, {"sampledAtUtc", sampled_at}, {"snapshot", snapshot}};
            for (const auto* error : {"first failed probe", "repeated failed probe"}) {
                record_scheduler_probe_failure(cache, 13, error);
                require(cache["snapshot"] == snapshot && cache["sampledAtUtc"] == sampled_at &&
                            cache["ok"] == false && cache["error"] == error &&
                            parse_utc(json_string(cache, "probeSampledAtUtc")).has_value() &&
                            snapshot_age(cache).value_or(0) >= 5000,
                        "Failed scheduler probes cannot relabel old queue data as fresh");
            }
            Json missing;
            record_scheduler_probe_failure(missing, 1, "No successful snapshot");
            require(missing["snapshot"].is_null() && missing["sampledAtUtc"].is_null() &&
                        !snapshot_age(missing) && missing["ok"] == false,
                    "An initial scheduler failure must not fabricate a sample or timestamp");
            const auto absent = scheduler_status_summary(missing);
            const auto expired = scheduler_status_summary(cache);
            require(absent["available"] == false && absent["stale"] == true && absent["ageMs"].is_null() &&
                        absent["occupied"].is_null() && absent["max_concurrent"].is_null() &&
                        absent["unavailableReason"] == "not_sampled" &&
                        absent["error"] == "No successful snapshot",
                    "Missing scheduler samples retain errors without fabricated queue counts");
            require(expired["available"] == false && expired["stale"] == true &&
                        expired["global_queued"].is_null() && expired["resource_capacity"].is_null() &&
                        expired["sampledAtUtc"] == sampled_at && expired["unavailableReason"] == "expired" &&
                        expired["error"] == "repeated failed probe",
                    "Expired queue data cannot masquerade as live capacity");
            const Json recovered{{"ok", true},
                                 {"sampledAtUtc", utc_now()},
                                 {"probeSampledAtUtc", utc_now()},
                                 {"snapshot", snapshot},
                                 {"error", nullptr}};
            const auto fresh = scheduler_status_summary(recovered);
            require(fresh["available"] == true && fresh["stale"] == false && fresh["occupied"] == 7 &&
                        fresh["global_queued"] == 3 && fresh["unavailableReason"].is_null() &&
                        fresh["error"].is_null(),
                    "A recovered fresh sample restores actual counts and clears the unavailable reason");
        }
        for (const auto& entry : tool_registry()["tools"])
            require(required_tool_scope(json_string(entry, "name")) == json_string(entry, "scope"),
                    "all tool scopes are derived from the canonical registry");
        require(capability_manifest(*config).size() == contract.all().size(), "one manifest entry per tool");
        require(contract.all().size() == cpp_tool_count,
                "embedded authoritative tool count plus native computer use");
        require(contract.tool("devbox_browser")["inputSchema"]["properties"]["max_bytes"]["maximum"] == 64000,
                "browser artifact bound is not enlarged by legacy file-transfer configuration");
        require(required_tool_scope("devbox_browser") == "mcp:host:exec" &&
                    contract.tool("devbox_browser")["annotations"]["readOnlyHint"] == false,
                "browser input retains execution authorization");
        require(required_tool_scope("devbox_chrome_bridge") == "mcp:host:exec" &&
                    contract.tool(
                        "devbox_chrome_bridge")["inputSchema"]["properties"]["max_bytes"]["maximum"] == 64000,
                "Chrome bridge retains execution scope and its bounded HTML contract");
        {
            const auto old_exe = environment("DEVBOX_CHROME_BRIDGE_EXE");
            const auto old_digest = environment("DEVBOX_CHROME_BRIDGE_SHA256");
            ScopeExit restore([&] {
                set_environment("DEVBOX_CHROME_BRIDGE_EXE", old_exe);
                set_environment("DEVBOX_CHROME_BRIDGE_SHA256", old_digest);
            });
            set_environment("DEVBOX_CHROME_BRIDGE_EXE", std::nullopt);
            require(data(chrome_bridge_call(*config, Json{{"action", "status"}}))["available"] == false,
                    "unconfigured bridge is explicitly unavailable");
            require(json_bool(chrome_bridge_call(*config, Json{{"action", "click"}}), "isError"),
                    "unconfigured input is refused without launching a process");
#ifdef _WIN32
            set_environment("DEVBOX_CHROME_BRIDGE_EXE", program);
            set_environment("DEVBOX_CHROME_BRIDGE_SHA256", std::string(64, '0'));
            require(data(chrome_bridge_call(*config, Json{{"action", "status"}}))["reason"] ==
                        "CHROME_BRIDGE_IDENTITY_MISMATCH",
                    "operator executable identity is checked before launch");
            set_environment("DEVBOX_CHROME_BRIDGE_SHA256", sha256_file(executable_path()));
            const auto bridge = data(chrome_bridge_call(*config, Json{{"action", "status"}}));
            require(bridge["ok"] == true && bridge["data"]["fixture"] == "chrome-bridge",
                    "pinned bridge uses structured stdin without a shell");
            const auto refused = chrome_bridge_call(*config, Json{{"action", "click"}});
            require(json_bool(refused, "isError") &&
                        refused["structuredContent"]["data"]["input_sent"] == false,
                    "structured nonzero bridge errors retain input-delivery evidence");
#endif
        }
        require(contract.tool("host_computer_windows")["annotations"]["readOnlyHint"] == true &&
                    contract.tool("host_computer_use")["annotations"]["readOnlyHint"] == false &&
                    contract.tool("host_computer_use")["annotations"]["idempotentHint"] == false,
                "computer input is a non-idempotent write action");
        require(required_tool_scope("host_computer_windows") == "mcp:host:read" &&
                    required_tool_scope("host_computer_use") == "mcp:host:exec",
                "computer-use calls cannot bypass OAuth tool scopes");
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
        require(required_tool_scope("devbox_status_summary") == "mcp:devbox:read" &&
                    contract.tool("devbox_status_summary")["annotations"]["readOnlyHint"] == true,
                "compact status uses the existing observation authorization");
        rejected = false;
        try {
            (void)contract.arguments("devbox_status_summary", Json{{"unknown", true}});
        } catch (const Error&) {
            rejected = true;
        }
        require(rejected, "compact status rejects unsupported arguments");
        config->max_wait_seconds = 2;
        require(
            ToolContract(*config).tool("devbox_wait")["inputSchema"]["properties"]["seconds"]["maximum"] == 2,
            "constrained schema");
        config->max_wait_seconds = 85;
        {
            constexpr std::uint64_t gib = 1024ULL * 1024 * 1024, floor = gib / 2;
            require(disk_pressure(6037696512ULL, 48274628608ULL, true) == "normal",
                    "observed Oracle volume with 12.5% free is not under disk pressure");
            require(disk_warning_free_bytes(40 * gib) == 4 * gib &&
                        disk_pressure(4 * gib, 40 * gib, true) == "normal" &&
                        disk_pressure(4 * gib - 1, 40 * gib, true) == "warning",
                    "small-volume warning begins below its capacity-aware byte allowance");
            require(disk_warning_free_bytes(40 * gib + 1) == 4 * gib + 1 &&
                        disk_pressure(4 * gib, 40 * gib + 1, true) == "warning" &&
                        disk_pressure(4 * gib + 1, 40 * gib + 1, true) == "normal",
                    "fractional-byte percentage thresholds round up without losing protection");
            require(disk_warning_free_bytes(2 * gib) == floor &&
                        disk_pressure(floor, 2 * gib, true) == "normal" &&
                        disk_pressure(floor - 1, 2 * gib, true) == "critical",
                    "the critical storage floor is unchanged on very small filesystems");
            require(disk_warning_free_bytes(500 * gib) == 50 * gib &&
                        disk_pressure(50 * gib, 500 * gib, true) == "normal" &&
                        disk_pressure(50 * gib - 1, 500 * gib, true) == "warning" &&
                        disk_pressure(60 * gib, 2000 * gib, true) == "warning",
                    "large-volume absolute and percentage warnings remain protective");
            const auto largest = std::numeric_limits<std::uint64_t>::max();
            require(disk_pressure(largest / 20, largest, true) == "warning" &&
                        disk_pressure(largest / 20 + 1, largest, true) == "normal",
                    "five-percent classification is exact at the uint64_t boundary");
            require(disk_pressure(100 * gib, 200 * gib, false) == "critical" &&
                        disk_pressure(gib, 0, true) == "critical" &&
                        disk_pressure(3 * gib, 2 * gib, true) == "critical",
                    "invalid space measurements remain fail-closed");
            const auto unknown = std::numeric_limits<std::uintmax_t>::max();
            require(!disk_space_sample_valid({unknown, unknown, unknown}, true) &&
                        !disk_space_sample_valid({40 * gib, 4 * gib, unknown}, true) &&
                        !disk_space_sample_valid({unknown, 4 * gib, 4 * gib}, true) &&
                        !disk_space_sample_valid({40 * gib, 4 * gib, 4 * gib}, false) &&
                        disk_space_sample_valid({40 * gib, 4 * gib, 4 * gib}, true),
                    "filesystem unknown-value sentinels are rejected at the probe boundary");
        }
        {
            const auto before = unix_millis();
            const auto old = snapshot_age(Json{{"sampledAtUtc", "1969-12-31T23:59:59.000Z"}});
            const auto after = unix_millis();
            require(old && *old >= before && *old <= after &&
                        snapshot_age(Json{{"sampledAtUtc", "2099-01-01T00:00:00.000Z"}}) == 0,
                    "Snapshot ages clamp pre-epoch and future wall-clock timestamps safely");
            for (const auto& invalid :
                 {Json(), Json::array(), Json(1), Json::object(), Json{{"sampledAtUtc", nullptr}},
                  Json{{"sampledAtUtc", 17}}, Json{{"sampledAtUtc", "invalid"}}})
                require(!snapshot_age(invalid),
                        "Missing or malformed snapshot timestamps remain unavailable");
            const Json named{{"sampledAtUtc", utc_now()}};
            const std::string extended_key = "sampledAtUtc-suffix";
            require(snapshot_age(named, std::string_view(extended_key).substr(0, 12)).has_value() &&
                        !snapshot_age(named, extended_key),
                    "Snapshot timestamp lookup respects bounded string views");
        }
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
            require(failed && monitor.store_health()["ok"] == false && !monitor.ready(cpp_tool_count),
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
            const auto health = monitor.store_health();
            require(health["warningFreeBytes"] == disk_warning_free_bytes(json_uint(health, "totalBytes")) &&
                        health["warningFreePercent"] == 5.0 && health["minimumFreeBytes"] == 536870912ULL,
                    "live storage status reports the actual warning allowance and unchanged critical floor");
            monitor.start();
            ScopeExit stop_monitor([&] { background.stop(); });
            const auto readiness_deadline = Clock::now() + Millis(10000);
            while (!monitor.ready(cpp_tool_count) && Clock::now() < readiness_deadline)
                std::this_thread::sleep_for(Millis(10));
            require(monitor.ready(cpp_tool_count) && !monitor.ready(cpp_tool_count - 1),
                    "Readiness requires a real fresh scheduler/store probe and the complete tool contract");
            background.failure("execution-store-probe", "readiness regression fixture");
            require(!monitor.ready(cpp_tool_count), "A failed background store probe rejects readiness");
            const auto incident_file = probe_config->project_root / "run" / "mcp-incidents.jsonl";
            const auto await_readiness_incident = [&](bool expected_ready) {
                const auto deadline = Clock::now() + Millis(12000);
                while (Clock::now() < deadline) {
                    if (fs::exists(incident_file)) {
                        const auto content = read_file(incident_file);
                        const auto complete = content.rfind('\n');
                        const auto lines = complete == std::string::npos
                                               ? std::vector<std::string>{}
                                               : split(content.substr(0, complete + 1), '\n', false);
                        for (const auto& line : lines) {
                            const auto incident = Json::parse(line);
                            const auto& trigger = incident.at("trigger");
                            if (trigger.contains("ready") && trigger.at("ready") == expected_ready &&
                                json_bool(trigger, "readinessChanged"))
                                return incident;
                        }
                    }
                    std::this_thread::sleep_for(Millis(25));
                }
                throw Error("Readiness transition did not preserve an incident");
            };
            const auto degraded = await_readiness_incident(false);
            require(degraded["backgroundTasks"]["execution-store-probe"]["lastError"] ==
                            "readiness regression fixture" &&
                        json_number(degraded["trigger"], "p95Ms") <= 100 &&
                        json_number(degraded["trigger"], "maxMs") <= 500,
                    "Fast readiness failure captures the failing worker without an event-loop incident");
            background.success("execution-store-probe");
            require(monitor.ready(cpp_tool_count), "A successful store probe restores readiness");
            const auto recovered = await_readiness_incident(true);
            require(recovered["backgroundTasks"]["execution-store-probe"]["consecutiveFailures"] == 0 &&
                        parse_utc(json_string(recovered, "observedAtUtc")).value() -
                                parse_utc(json_string(degraded, "observedAtUtc")).value() <
                            30000,
                    "Recovery is retained inside the prior incident cooldown");
            background.mark_stopped("execution-store-probe");
            require(!monitor.ready(cpp_tool_count), "A stopped store probe rejects readiness");
            background.stop();
            stop_monitor.disarm();
            usage.stop();
        }
        {
            (void)operator_admission(*config, "drain");
            AdmissionControl admission(*config);
            require(admission.permits("devbox_status_summary", Json::object()) &&
                        !admission.permits("devbox_run_program", Json::object()),
                    "draining permits compact observation while refusing new execution");
            (void)operator_admission(*config, "resume");
        }
        auto engine = std::make_shared<Engine>(config);
        HttpServer server(config, engine);
        const auto base = "http://127.0.0.1:" + std::to_string(server.start());
        engine->attach(server);
        ScopeExit stop([&] {
            server.stop();
            engine->stop();
        });
        require(engine->list_tools("2025-11-25").size() == cpp_tool_count &&
                    json_bool(engine->parity_report(), "complete") &&
                    json_bool(engine->parity_report(), "cutover_allowed"),
                "complete native implementation advertises all legacy and computer-use tools");
        const auto capabilities = data(invoke(base, "devbox_capabilities"));
        require(capabilities["implementation"] == "cpp" && capabilities["tools"].size() == cpp_tool_count &&
                    capabilities["contract_version"] == cpp_contract_version &&
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
        const auto batch =
            data(invoke(base, "devbox_read_files", Json{{"paths", Json::array({path_text(file)})}}));
        require(batch["complete"] == true && batch["files"][0]["text"] == "alpha\nbeta\n" &&
                    batch["files"][0]["read_bytes_sha256"] == sha256("alpha\nbeta\n"),
                "native batched file read reaches the isolated worker through MCP");
        const auto partial_batch =
            invoke(base, "devbox_read_files",
                   Json{{"paths", Json::array({path_text(root / "missing-batch"), path_text(file)})}});
        require(json_bool(partial_batch, "isError") &&
                    partial_batch["structuredContent"]["data"]["errors"] == 1 &&
                    partial_batch["structuredContent"]["data"]["files"][1]["text"] == "alpha\nbeta\n",
                "partial batched reads are errors with successful file evidence retained");
        require(json_bool(invoke(base, "devbox_read_files", Json{{"paths", Json::array()}}), "isError"),
                "batch schema rejects an empty path list");
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
        require(inspect["resolved_path"] == path_text(file) && inspect["exists"] == true &&
                    inspect["utf8_valid"] == true,
                "host file aliases inspect the native path on every platform");
        const auto host_root = root / "host-alias";
        const auto host_file = host_root / "exact.bin";
        fs::create_directories(host_root);
        const auto written = data(invoke(base, "windows_host_write_large_file",
                                         Json{{"path", "nested/../exact.bin"},
                                              {"working_dir", path_text(host_root)},
                                              {"content_base64", base64_encode("host-alias")}}));
        require(written["path"] == path_text(host_file) && written["verified"] == true &&
                    read_file(host_file) == "host-alias",
                "host write alias produces exact bytes at the native normalized path");
        const auto host_read =
            data(invoke(base, "windows_host_read_large_file",
                        Json{{"path", path_text(host_file)}, {"offset_bytes", 5}, {"max_bytes", 5}}));
        require(host_read["path"] == path_text(host_file) &&
                    host_read["content_base64"] == base64_encode("alias"),
                "host read alias addresses the same native file");
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
        const auto compact_response = invoke(base, "devbox_status_summary");
        const auto compact = data(compact_response);
        require(compact["view"] == "summary" && compact["detailTool"] == "devbox_status" &&
                    compact["build"] == status["build"] &&
                    compact["performance"]["process"]["pid"] == process_id() &&
                    compact["capabilities"]["schema_sha256"] == capabilities["schema_sha256"],
                "compact MCP status retains actual binary and contract identity");
        require(compact["executionStore"]["ok"] == status["executionStore"]["ok"] &&
                    compact["executionStore"]["diskPressure"] == status["executionStore"]["diskPressure"] &&
                    compact["operationalWarnings"] == status["operationalWarnings"] &&
                    compact["degradedSubsystems"] == status["degradedSubsystems"] &&
                    compact["guardian"] == status["guardian"] &&
                    compact["admission"] == status["admission"] &&
                    compact["capabilities"]["execution_profiles"] == capabilities["execution_profiles"] &&
                    compact["capabilities"]["platform_availability"] == capabilities["platform_availability"],
                "compact status does not hide health, admission, or isolation limitations");
        require(compact["execution"]["max_concurrent"] == status["execution"]["max_concurrent"] &&
                    compact["execution"]["occupied"] == status["execution"]["occupied"] &&
                    compact["execution"].contains("global_queued") && compact["ready"].is_boolean() &&
                    parse_utc(json_string(compact["execution"], "sampledAtUtc")).has_value() &&
                    parse_utc(json_string(compact["execution"], "probeSampledAtUtc")).has_value() &&
                    json_uint(compact["execution"], "ageMs") < 5000 &&
                    compact["execution"]["available"] == true && compact["execution"]["stale"] == false &&
                    compact["execution"]["ok"] == true && compact["execution"]["error"].is_null() &&
                    parse_utc(json_string(compact, "observedAtUtc")).has_value() &&
                    compact["performance"].contains("stale") &&
                    compact["executionStore"].contains("sampledAtUtc"),
                "compact status carries queue occupancy, readiness and measurement freshness");
        require(!compact.contains("backgroundTasks") && !compact.contains("jobMaintenance") &&
                    !compact.contains("jobQuota") && !compact["capabilities"].contains("tools") &&
                    !compact["capabilities"].contains("tool_manifest") &&
                    status["capabilities"]["tool_manifest"].size() == cpp_tool_count &&
                    !status.contains("view"),
                "compact status is opt-in and the frozen detailed action remains complete");
        require(compact.dump().size() * 2 < status.dump().size(),
                "routine compact status is less than half of the detailed structured payload");
        const auto compact_text = compact_response["content"][0]["text"].get<std::string>();
        const auto json_start = compact_text.find("\n\n");
        require(json_start != std::string::npos &&
                    Json::parse(compact_text.substr(json_start + 2)) == compact,
                "model-visible compact JSON agrees exactly with structured content");
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
