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
    auto values = current_environment();
    const auto set = [&](std::string key, std::string value) {
#ifdef _WIN32
        std::erase_if(values, [&](const auto& item) { return lower(item.first) == lower(key); });
#endif
        values[std::move(key)] = std::move(value);
    };
    set("DEVBOX_PROJECT_ROOT", path_text(config.project_root));
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
    auto request = shell_request(options, resource, read_only);
    ensure_directory(config_->jobs_root);
    FileLock gate(config_->jobs_root / ".submission.lock");
    store_.admit();
    return persist_and_spawn(std::move(request), {});
}
Json JobManager::start_program(const ProgramRequest& options, std::string_view resource) {
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
    try {
        pid = spawn_detached(executable_path(), {"--job-runner", path_text(paths.request)},
                             config_->project_root, runner_environment(*config_));
    } catch (const std::exception& error) {
        initial["status"] = "failed";
        initial["completedAtUtc"] = utc_now();
        initial["error"] = "Failed to launch detached C++ job runner: " + std::string(error.what());
        store_.write_status(id, initial);
        throw;
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
Json JobManager::submit(Json request, const Submission& agent) {
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
    const auto receipt_path = config_->jobs_root / ".operations" / path_from_utf8(id + ".json");
    if (const auto receipt = read_json_optional(receipt_path, 65536)) {
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
    const auto receipts = config_->jobs_root / ".operations";
    std::size_t count = 0;
    if (fs::exists(receipts))
        for (const auto& entry : fs::directory_iterator(receipts)) {
            (void)entry;
            if (++count >= config_->job_max_operations)
                throw Error("OPERATION_CAPACITY: durable operation receipts are full; archive the task "
                            "records explicitly before accepting new operation IDs");
        }
    store_.admit(agent.task_id);
    Json receipt{{"id", id},
                 {"agent", agent.json()},
                 {"fingerprint", fingerprint},
                 {"submitted", false},
                 {"createdAtUtc", request["createdAtUtc"]}};
    atomic_write(receipt_path, receipt.dump());
    const auto summary = persist_and_spawn(std::move(request), agent);
    receipt["submitted"] = true;
    atomic_write(receipt_path, receipt.dump());
    return Json{{"id", id}, {"replayed", false}, {"agent", agent.json()}, {"job", summary}};
}
} // namespace devbox
