#include "devbox/jobs.hpp"
#include <algorithm>
namespace devbox {
namespace {
std::string new_id() {
    return "job-" + std::to_string(unix_millis()) + '-' + uuid().substr(0, 12);
}
std::uint64_t bounded_timeout(Millis timeout) {
    return static_cast<std::uint64_t>(std::clamp<Millis::rep>(timeout.count(), 1000, 86400000));
}
Environment runner_environment(const Config& config) {
    auto values = config.runtime_mode == RuntimeMode::docker ? docker_environment() : worker_environment();
    const auto set = [&](std::string key, std::string value) {
#ifdef _WIN32
        std::erase_if(values, [&](const auto& item) { return lower(item.first) == lower(key); });
#endif
        values[std::move(key)] = std::move(value);
    };
    set("DEVBOX_PROJECT_ROOT", path_text(config.project_root));
    if (const auto generation = environment("DEVBOX_DEPLOYMENT_GENERATION"))
        set("DEVBOX_DEPLOYMENT_GENERATION", *generation);
    set("DEVBOX_RUNTIME_MODE", config.runtime_name());
    set("ENABLE_HOST_EXEC", config.host_exec_enabled ? "true" : "false");
    set("HOST_DEFAULT_WORKDIR", path_text(config.host_default_workdir));
    set("HOST_WORKSPACE_PATH", path_text(config.host_workspace_path));
    set("DEVBOX_WORKSPACE_PATH", path_text(config.devbox_workspace_path));
    set("DEVBOX_CONTAINER_NAME", config.devbox_container_name);
    set("DEVBOX_DEFAULT_USER", config.devbox_default_user);
    set("HOST_SHELL", config.host_shell);
    set("NODE_EXE", config.node_exe);
    set("POWERSHELL_EXE", config.power_shell_exe);
    set("POWERSHELL_FALLBACK_EXE", config.power_shell_fallback_exe);
    set("DEVBOX_PROGRAM_ALLOWLIST", join(config.devbox_program_allowlist, ","));
    set("HOST_PROGRAM_ALLOWLIST", join(config.host_program_allowlist, ","));
    set("HOST_PROGRAM_ALLOWLIST_REPLACE", "true");
    set("HOST_PROGRAM_ALLOWLIST_EXTRA", "");
    set("MCP_JOBS_ROOT", path_text(config.jobs_root));
    set("MCP_STATE_BACKEND", config.state_backend);
    set("MCP_STATE_ROOT",
        path_text(config.state_root.empty() ? config.project_root / "run" / "state" : config.state_root));
    set("MCP_EXEC_SLOT_ROOT", path_text(config.execution_slot_root));
    set("MCP_BACKGROUND_QUEUE_TIMEOUT_MS", std::to_string(config.background_queue_timeout_ms));
    set("MCP_EXEC_MAX_CONCURRENT", std::to_string(config.exec_max_concurrent));
    set("MCP_EXEC_RESERVED_INTERACTIVE", std::to_string(config.exec_reserved_interactive));
    set("MCP_WATCH_MAX_CONCURRENT", std::to_string(config.watch_max_concurrent));
    set("MCP_EXEC_HEAVY_CAPACITY", std::to_string(config.exec_heavy_capacity));
    set("MCP_EXEC_HEAVY_WEIGHT", std::to_string(config.exec_heavy_weight));
    set("MCP_EXEC_IO_HEAVY_CAPACITY", std::to_string(config.exec_io_heavy_capacity));
    set("MCP_EXEC_IO_HEAVY_WEIGHT", std::to_string(config.exec_io_heavy_weight));
    set("MCP_BACKGROUND_PRIORITY_AGE_MS", std::to_string(config.background_priority_age_ms));
    set("MCP_JOB_LOG_MAX_BYTES", std::to_string(config.job_log_max_bytes));
    set("MCP_JOB_LOG_ROTATIONS", std::to_string(config.job_log_rotations));
    set("MCP_JOB_HEARTBEAT_MS", std::to_string(config.job_heartbeat_ms));
    set("MCP_JOB_ORPHAN_STALE_MS", std::to_string(config.job_orphan_stale_ms));
    set("MAX_COMMAND_OUTPUT_CHARS", std::to_string(config.command_output_limit_chars));
    return values;
}
} // namespace
Environment background_environment(const Config& config) {
    return runner_environment(config);
}
Json JobManager::shell_request(const ShellRequest& options, std::string_view resource, bool read_only) const {
    return Json{{"id", new_id()},
                {"mode", "shell"},
                {"command", options.command},
                {"args", Json::array()},
                {"workingDir", path_text(options.working_dir)},
                {"timeoutMs", bounded_timeout(options.timeout)},
                {"user", options.user},
                {"readOnly", read_only},
                {"resourceClass", resource_name(infer_shell_resource(options.command, resource))},
                {"runtimeMode", config_->runtime_name()},
                {"createdAtUtc", utc_now()}};
}
Json JobManager::program_request(const ProgramRequest& options, std::string_view resource) const {
    Json request{{"id", new_id()}, {"mode", "program"}, {"program", options.program}, {"args", options.args}};
    if (options.input)
        request["input"] = *options.input;
    request["workingDir"] = path_text(options.working_dir);
    request["timeoutMs"] = bounded_timeout(options.timeout);
    request["user"] = options.user;
    request["readOnly"] = false;
    request["resourceClass"] = resource_name(infer_program_resource(options.program, options.args, resource));
    request["runtimeMode"] = config_->runtime_name();
    request["createdAtUtc"] = utc_now();
    return request;
}
Json JobManager::start_shell(const ShellRequest& options, std::string_view resource, bool read_only) {
    store_.require_writable_backend();
    auto request = shell_request(options, resource, read_only);
    ensure_directory(config_->jobs_root);
    FileLock gate(config_->jobs_root / ".submission.lock");
    store_.admit();
    return persist_and_spawn(std::move(request), {});
}
Json JobManager::start_program(const ProgramRequest& options, std::string_view resource) {
    store_.require_writable_backend();
    auto request = program_request(options, resource);
    ensure_directory(config_->jobs_root);
    FileLock gate(config_->jobs_root / ".submission.lock");
    store_.admit();
    return persist_and_spawn(std::move(request), {});
}
Json JobManager::persist_and_spawn(Json request, const std::optional<Submission>& agent) {
    Json initial{{"id", request["id"]},
                 {"status", "queued"},
                 {"mode", request["mode"]},
                 {"createdAtUtc", request["createdAtUtc"]},
                 {"startedAtUtc", nullptr},
                 {"completedAtUtc", nullptr},
                 {"runnerPid", nullptr},
                 {"exitCode", nullptr},
                 {"readOnly", request["readOnly"]},
                 {"resourceClass", request["resourceClass"]},
                 {"runtimeMode", request["runtimeMode"]}};
    if (agent)
        request["agent"] = agent->json();
    const auto id = request["id"].get<std::string>();
    const auto paths = store_.create_job(id, request, initial);
    std::uint32_t pid;
    std::optional<std::uint64_t> instance;
    try {
        pid = spawn_detached(executable_path(), {"--job-runner", path_text(paths.request)},
                             config_->project_root, runner_environment(*config_), &instance);
    } catch (const std::exception& error) {
        initial["status"] = "failed";
        initial["completedAtUtc"] = utc_now();
        initial["error"] = "Failed to launch detached C++ job runner: " + std::string(error.what());
        store_.write_status(id, initial);
        throw;
    }
    // Publish launch identity independently of the runner's mutable status. A cold-starting but
    // live process must not be called orphaned before it has written its first heartbeat.
    try {
        write_json_atomic(paths.dir / "runner-owner.json",
                          Json{{"id", id},
                               {"pid", pid},
                               {"instance", instance ? Json(std::to_string(*instance)) : Json()}});
    } catch (...) {
        throw Error("RUNNER_OWNERSHIP_UNCONFIRMED: a process was started; retain the operation identity and "
                    "inspect its job before retrying");
    }
    return Json{{"id", id},
                {"status", "queued"},
                {"runnerPid", pid},
                {"jobDir", path_text(paths.dir)},
                {"mode", request["mode"]},
                {"resourceClass", request["resourceClass"]}};
}
Json JobManager::submit_shell(const ShellRequest& options, const Submission& agent, std::string_view resource,
                              bool read_only) {
    return submit(shell_request(options, resource, read_only), agent);
}
Json JobManager::submit_program(const ProgramRequest& options, const Submission& agent,
                                std::string_view resource) {
    return submit(program_request(options, resource), agent);
}
Json JobManager::submit_research(const Json& plan, const Submission& agent) {
    Json request{{"id", new_id()},
                 {"mode", "research"},
                 {"research", plan},
                 {"workingDir", path_text(config_->project_root)},
                 {"timeoutMs", json_uint(plan, "budget_seconds", 300) * 1000},
                 {"readOnly", true},
                 {"resourceClass", "io-heavy"},
                 {"runtimeMode", config_->runtime_name()},
                 {"createdAtUtc", utc_now()}};
    return submit(std::move(request), agent);
}
Json JobManager::submit(Json request, const Submission& agent) {
    store_.require_writable_backend();
    validate_key(agent.task_id);
    validate_key(agent.operation_id);
    if (agent.label.size() > 200)
        throw Error("label exceeds 200 bytes");
    const auto id = "job-op-" + sha256(Json::array({agent.task_id, agent.operation_id}).dump());
    request["id"] = id;
    auto identity = request;
    identity.erase("createdAtUtc");
    identity["agent"] = agent.json();
    const auto fingerprint = sha256(canonical_json(identity).dump());
    ensure_directory(config_->jobs_root);
    FileLock gate(config_->jobs_root / ".submission.lock");
    if (const auto receipt = store_.operation_receipt(id)) {
        if (json_string(*receipt, "fingerprint") != fingerprint)
            throw Error("OPERATION_CONFLICT: operation ID already belongs to a different request");
        Json status;
        try {
            status = store_.get_status(id);
        } catch (const std::exception& error) {
            if (fs::exists(store_.paths(id).dir))
                throw Error("JOB_STATE_UNAVAILABLE: retained operation will not be executed again: " +
                            std::string(error.what()));
            status =
                Json{{"id", id},
                     {"status", json_bool(*receipt, "submitted") ? "result_expired" : "submission_unknown"},
                     {"message", "Operation ID is retained and will not be executed again. Inspect the task "
                                 "before choosing a new operation ID."}};
        }
        return Json{{"id", id}, {"replayed", true}, {"agent", agent.json()}, {"job", status}};
    }
    if (store_.operation_count() >= config_->job_max_operations)
        throw Error("OPERATION_CAPACITY: durable operation receipts are full; archive the task records "
                    "explicitly before accepting new operation IDs");
    store_.admit(agent.task_id);
    Json receipt{{"id", id},
                 {"agent", agent.json()},
                 {"fingerprint", fingerprint},
                 {"submitted", false},
                 {"createdAtUtc", request["createdAtUtc"]}};
    store_.write_operation_receipt(id, receipt);
    const auto summary = persist_and_spawn(std::move(request), agent);
    receipt["submitted"] = true;
    store_.write_operation_receipt(id, receipt);
    return Json{{"id", id}, {"replayed", false}, {"agent", agent.json()}, {"job", summary}};
}
} // namespace devbox
