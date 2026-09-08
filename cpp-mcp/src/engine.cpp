#include "devbox/engine.hpp"
#include "devbox/result.hpp"
#include <algorithm>
namespace devbox {
namespace {
std::string host_title(const Config& config) {
    return config.platform.is_windows ? "Windows Host" : config.platform.display_name + " Host";
}
std::optional<std::string> optional_string(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? std::optional(value[key].get<std::string>())
                                                         : std::nullopt;
}
} // namespace
Engine::Engine(std::shared_ptr<const Config> config)
    : config_(std::move(config)), contract_(*config_),
      commands_(std::clamp<std::size_t>(config_->exec_max_concurrent + 1, 2, 16), 128), runtime_(config_),
      scheduler_(SchedulerConfig::from(*config_)), jobs_(config_), docker_files_(config_), search_(config_),
      lifecycle_(config_, background_), github_(config_, runtime_, lifecycle_), capture_(config_),
      usage_(*config_, background_), performance_(*config_, background_, build_snapshot),
      monitoring_(config_, background_, scheduler_, jobs_.store(), runtime_, performance_, usage_,
                  [this] { return server_ ? server_->active_requests() : 0; }) {
    for (const auto& tool : contract_.all()) {
        const auto name = json_string(tool, "name");
        implemented_.insert(name);
    }
}
Engine::~Engine() {
    stop();
}
void Engine::attach(HttpServer& server) {
    if (server_)
        throw Error("Engine is already attached");
    server_ = &server;
    // Compute the binary digest outside the HTTP I/O executor before accepting tool work.
    (void)build_snapshot();
    performance_.attach(server.executor(), server.stop_token());
    monitoring_.start();
    if (config_->devbox_auto_start)
        background_.once("runtime-auto-start", Millis(0), [this](const Cancel& cancel) {
            lifecycle_.control(LifecycleAction::start, cancel);
        });
}
void Engine::stop() {
    if (stopped_.exchange(true))
        return;
    background_.stop();
    usage_.stop();
}
Json Engine::server_info() const {
    return Json{{"name", config_->server_name()},
                {"version", build_version()},
                {"websiteUrl", "https://github.com/adybag14-cyber/devbox"}};
}
Json Engine::list_tools(std::string_view protocol) const {
    (void)protocol;
    return contract_.selected(implemented_);
}
Json Engine::parity_report() const {
    Json remaining = Json::array();
    for (const auto& tool : contract_.all())
        if (!implemented_.contains(json_string(tool, "name")))
            remaining.push_back(tool["name"]);
    return Json{{"implementation", "cpp"},
                {"contract_version", 2},
                {"implemented_tools", implemented_.size()},
                {"target_tools", contract_.all().size()},
                {"remaining_tools", remaining},
                {"complete", false},
                {"cutover_allowed", false},
                {"remaining_gates",
                 {"cross-platform-runtime-certification", "native-desktop-capture-certification",
                  "managed-launcher-certification", "release-packaging"}},
                {"build", build_snapshot()}};
}
bool Engine::ready() const {
    return monitoring_.ready(implemented_.size());
}
std::string Engine::tool_started(const std::string& name, const Json& args, const Json& context) {
    return usage_.started(name, args, context);
}
void Engine::tool_finished(const std::string& id, const Json& result) {
    usage_.finished(id, result);
}
void Engine::tool_failed(const std::string& id, const std::string& error) {
    usage_.failed(id, error);
}
void Engine::observe_http(const HttpRequest& request, int status, std::uint64_t bytes, Millis duration,
                          bool disconnected) {
    (void)bytes;
    usage_.http(request, status, duration, disconnected);
}
fs::path Engine::working_dir(const Json& args, bool host) const {
    const auto dir = trim(json_string(args, "working_dir"));
    return dir.empty() ? (host ? config_->host_default_workdir : config_->devbox_workspace_path)
                       : path_from_utf8(dir);
}
void Engine::require_agent_host() const {
    if (config_->runtime_mode != RuntimeMode::host)
        throw Error("Durable host APIs require host runtime");
    if (!config_->host_exec_enabled)
        throw Error("Host execution is disabled");
}
asio::awaitable<Json> Engine::call_tool(std::string name, Json arguments, Cancel cancel) {
    if (!cancel)
        cancel = std::make_shared<Cancellation>();
    try {
        if (!implemented_.contains(name))
            throw Error("Unknown tool: " + name);
        auto args = contract_.arguments(name, arguments);
        if (name.find("capture") != name.npos)
            co_return co_await capture(name, std::move(args), cancel);
        if (name == "devbox_wait") {
            const auto start = Clock::now();
            const auto seconds = json_number(args, "seconds");
            try {
                co_await async_delay(Millis(static_cast<Millis::rep>(seconds * 1000)), cancel);
            } catch (const Cancelled&) {
                co_return result_error("Wait was cancelled.");
            }
            co_return result_success(
                "Waited " + args["seconds"].dump() + " seconds without an execution process.",
                Json{{"waited_ms", std::chrono::duration_cast<Millis>(Clock::now() - start).count()},
                     {"reason", json_string(args, "reason").empty() ? Json() : args["reason"]}});
        }
        if (name == "devbox_wait_for_file")
            co_return co_await wait_file(std::move(args), cancel);
        if (name == "devbox_job_status")
            co_return co_await wait_job(std::move(args), cancel);
        if (name == "devbox_exec" || name == "devbox_exec_readonly" || name == "devbox_run_program" ||
            name == "host_exec" || name == "windows_host_exec" || name == "host_run_program" ||
            name == "windows_host_run_program")
            co_return co_await execute(name, std::move(args), cancel);
        if (name == "devbox_search_files")
            co_return co_await search(std::move(args), cancel);
        if (name == "devbox_exec_start" || name == "devbox_run_program_start" ||
            name == "devbox_job_submit") {
            auto pending = controls_.run([this, name, args] { return detached(name, args); }, cancel);
            co_return co_await std::move(pending);
        }
        if (name == "devbox_status") {
            auto pending = controls_.run([this, cancel] { return status(cancel); }, cancel);
            co_return co_await std::move(pending);
        }
        if (name == "host_status" || name == "windows_host_status")
            co_return host_status();
        if (name == "devbox_start" || name == "devbox_stop" || name == "devbox_restart" ||
            name == "devbox_recreate") {
            auto pending = controls_.run([this, name, cancel] { return lifecycle(name, cancel); }, cancel);
            co_return co_await std::move(pending);
        }
        if (name == "devbox_github_auth_status" || name == "devbox_sync_github_auth_from_host") {
            auto pending = commands_.run(
                [this, name, cancel] {
                    try {
                        if (name == "devbox_github_auth_status")
                            return result_success("Fetched " + config_->runtime_label() +
                                                      " GitHub auth status.",
                                                  github_.status(cancel));
                        const auto value = github_.sync_from_host(cancel);
                        auto data = value["status"];
                        for (const auto* key : {"hostUserName", "hostUserEmail"})
                            data[key] = json_string(value, key).empty() ? Json() : value[key];
                        return result_success("Synced the host GitHub CLI authentication into the " +
                                                  config_->runtime_label() + ".",
                                              data);
                    } catch (const std::exception& e) {
                        return render_process_error(e, config_->command_output_limit_chars);
                    }
                },
                cancel);
            co_return co_await std::move(pending);
        }
        if (name == "devbox_job_logs" || name == "devbox_job_cancel") {
            auto pending = controls_.run(
                [this, name, args] {
                    const auto id = json_string(args, "job_id");
                    try {
                        if (name == "devbox_job_logs")
                            return result_success(
                                "Read logs for background job " + id + ".",
                                jobs_.store().logs(id, json_uint(args, "max_chars", 20000)));
                        auto value = jobs_.store().cancel(id);
                        return result_success("Background job " + id + " is " +
                                                  json_string(value, "status", "unknown") + ".",
                                              value);
                    } catch (const std::exception& e) {
                        return result_error(std::string(name == "devbox_job_logs"
                                                            ? "Failed to fetch logs for detached job "
                                                            : "Failed to cancel detached job ") +
                                            id + ": " + e.what());
                    }
                },
                cancel);
            co_return co_await std::move(pending);
        }
        if (name == "devbox_file_state" || name == "devbox_write_file_atomic" || name == "devbox_task_get" ||
            name == "devbox_task_put" || name == "devbox_task_list" || name == "devbox_job_list" ||
            name == "devbox_capabilities") {
            auto& pool = name == "devbox_write_file_atomic" ? atomic_ : files_;
            auto pending = pool.run([this, name, args] { return durable(name, args); }, cancel);
            co_return co_await std::move(pending);
        }
        auto& pool = name.find("write") != name.npos ? atomic_ : files_;
        auto pending = pool.run([this, name, args, cancel] { return files(name, args, cancel); }, cancel);
        co_return co_await std::move(pending);
    } catch (const ParameterError& e) {
        co_return Json{{"content", Json::array({Json{{"type", "text"}, {"text", e.what()}}})},
                       {"isError", true}};
    } catch (const std::exception& e) {
        co_return result_error(e.what());
    }
}
Json Engine::durable(std::string name, const Json& args) {
    Json value;
    std::string summary;
    if (name == "devbox_capabilities") {
        value = optional_string(args, "tool_name") ? contract_.tool(json_string(args, "tool_name"))
                                                   : contract_.capabilities(*config_, implemented_);
        summary = "Read native C++ capabilities.";
    } else if (name == "devbox_job_list") {
        value = jobs_.store().list(optional_string(args, "task_id"), json_strings(args, "statuses"),
                                   optional_string(args, "cursor"), json_uint(args, "limit", 50));
        summary = "Listed durable jobs.";
    } else if (name.starts_with("devbox_task_")) {
        const auto root = config_->project_root / "run" / "tasks";
        if (name == "devbox_task_get") {
            value = task_get(root, json_string(args, "task_id"));
            summary = "Read task checkpoint.";
        } else if (name == "devbox_task_put") {
            value = task_put(root, json_string(args, "task_id"), json_uint(args, "expected_revision"),
                             args["state"]);
            summary = "Saved task checkpoint.";
        } else {
            value = task_list(root, optional_string(args, "cursor"), json_uint(args, "limit", 50));
            summary = "Listed task checkpoints.";
        }
    } else {
        require_agent_host();
        const auto path = path_from_utf8(json_string(args, "path"));
        const auto resolved = path.is_absolute() ? path : config_->devbox_workspace_path / path;
        if (name == "devbox_file_state") {
            value = file_state(resolved).json();
            summary = "Read file version.";
        } else {
            const auto encoded = json_string(args, "content_base64");
            if (encoded.size() > config_->max_mcp_transfer_chars)
                throw Error("Payload exceeds transfer limit");
            const bool append = json_bool(args, "append");
            if (append &&
                (!args.contains("expected_offset_bytes") || args["expected_offset_bytes"].is_null()))
                throw Error("Atomic append requires expected_offset_bytes");
            const auto payload = base64_decode(encoded);
            if (base64_encode(payload) != encoded)
                throw Error("content_base64 must be canonical");
            Preconditions expected;
            expected.sha256 = json_string(args, "expected_file_sha256");
            if (args.contains("expected_offset_bytes") && args["expected_offset_bytes"].is_number())
                expected.offset = json_uint(args, "expected_offset_bytes");
            value =
                atomic_write(resolved,
                             std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()),
                             append, json_bool(args, "create_dirs", true), expected)
                    .json();
            summary = "Committed atomic file write.";
        }
    }
    return result_explicit(summary, value, summary + "\n\n" + value.dump());
}
Json Engine::detached(std::string name, const Json& args) {
    const bool agent = name == "devbox_job_submit";
    const auto command = optional_string(args, "command"), program = optional_string(args, "program");
    if (agent) {
        require_agent_host();
        if (program.has_value() == command.has_value() || (program && trim(*program).empty()) ||
            (command && trim(*command).empty()))
            throw Error("Exactly one non-empty program or command is required");
        if (json_strings(args, "args").size() > 256)
            throw Error("Invalid job timeout or argument count");
        if (command && (!json_strings(args, "args").empty() || optional_string(args, "input")))
            throw Error("args/input require a program job");
    }
    const bool shell = command.has_value();
    const auto resource = json_string(args, "resource_class", "auto");
    const auto klass =
        shell ? infer_shell_resource(*command, resource)
              : infer_program_resource(program.value_or(""), json_strings(args, "args"), resource);
    const auto operation =
        shell ? *command : program.value_or("") + " " + join(json_strings(args, "args"), " ");
    if (auto refusal = monitoring_.reject_disk_work(klass, shell && json_bool(args, "read_only"), operation))
        return *refusal;
    const auto timeout = Millis(json_uint(args, "timeout_seconds", 7200) * 1000);
    const auto user = json_string(args, "user", config_->devbox_default_user);
    Submission submission{json_string(args, "task_id"), json_string(args, "operation_id"),
                          json_string(args, "label")};
    Json value;
    try {
        if (shell) {
            ShellRequest request;
            request.command = *command;
            request.working_dir = working_dir(args, false);
            request.timeout = timeout;
            request.user = user;
            value = agent ? jobs_.submit_shell(request, submission, resource, json_bool(args, "read_only"))
                          : jobs_.start_shell(request, resource, json_bool(args, "read_only"));
        } else {
            ProgramRequest request;
            request.program = trim(program.value_or(""));
            request.args = json_strings(args, "args");
            request.input = optional_string(args, "input");
            request.working_dir = working_dir(args, false);
            request.timeout = timeout;
            request.user = user;
            value = agent ? jobs_.submit_program(request, submission, resource)
                          : jobs_.start_program(request, resource);
        }
    } catch (const std::exception& e) {
        return result_error(agent ? e.what()
                                  : std::string("Failed to start detached ") + (shell ? "shell" : "program") +
                                        " job: " + e.what());
    }
    if (agent)
        return result_explicit("Submitted or recovered durable job.", value,
                               "Submitted or recovered durable job.\n\n" + value.dump());
    return result_success(std::string(shell ? "Started background " : "Started direct background ") +
                              config_->runtime_label() + " job " + json_string(value, "id") + ".",
                          value);
}
Json Engine::status(const Cancel& cancel) {
    Json info;
    try {
        info = lifecycle_.status(cancel);
    } catch (const std::exception& e) {
        return result_error("Failed to fetch " + config_->runtime_label() + " status: " + e.what());
    }
    const auto execution = monitoring_.execution();
    if (!execution)
        return result_error("Failed to fetch " + config_->runtime_label() +
                            " status: cached scheduler snapshot is unavailable or stale.");
    auto data = info;
    data["hostWorkspacePath"] = path_text(config_->host_workspace_path);
    data["devboxWorkspacePath"] = path_text(config_->devbox_workspace_path);
    data["hostExecEnabled"] = config_->host_exec_enabled;
    data["guardian"] = guardian_snapshot(*config_);
    try {
        data["startup"] =
            read_json_optional(config_->project_root / "run" / "startup-state.json").value_or(Json());
    } catch (...) {
        data["startup"] = nullptr;
    }
    data["jobMaintenance"] = read_status_snapshot(config_->project_root / "run" / "job-maintenance.json");
    data["jobQuota"] = read_status_snapshot(config_->project_root / "run" / "job-quota.json");
    data["execution"] = *execution;
    data["capabilities"] = contract_.capabilities(*config_, implemented_);
    data["performance"] = performance_.snapshot();
    const auto bg = background_.snapshot();
    data["backgroundTasks"] = bg;
    const auto health = monitoring_.store_health();
    data["executionStore"] = health;
    data["operationalWarnings"] =
        json_string(health, "diskPressure") == "warning" ? Json::array({"disk-pressure"}) : Json::array();
    data["degradedSubsystems"] = degraded_background(bg);
    data["usageTelemetry"] = usage_.snapshot();
    const auto active = server_ ? server_->active_requests() : 0;
    data["activeRequestsIncludingCurrent"] = active;
    data["activeRequests"] = active ? active - 1 : 0;
#ifdef _WIN32
    data["processProbe"] = process_probe_metrics();
#endif
    if (json_bool(info, "running")) {
        const auto versions = runtime_.cached_versions();
        data["versions"] = versions ? Json(*versions) : Json();
        data["versionsCached"] = versions.has_value();
    }
    return result_success("Fetched " + config_->runtime_label() + " status.", data);
}
Json Engine::host_status() const {
    const auto node = find_program(config_->node_exe);
    return result_success(
        "Fetched " + lower(host_title(*config_)) + " tool status.",
        Json{{"enabled", config_->host_exec_enabled},
             {"platform", config_->platform.id},
             {"platformDisplayName", config_->platform.display_name},
             {"shell", config_->host_shell},
             {"defaultWorkdir", path_text(config_->host_default_workdir)},
             {"allowlist", config_->host_program_allowlist},
             {"resolvedNodeExe", node ? path_text(*node) : config_->node_exe},
             {"powerShellExe", config_->power_shell_exe},
             {"powerShellFallbackExe", config_->power_shell_fallback_exe},
             {"powerShellFallbackEnabled", !config_->power_shell_fallback_exe.empty() &&
                                               config_->power_shell_fallback_exe != config_->power_shell_exe},
             {"windowsHostExecDefaultsToAdmin", config_->platform.is_windows},
             {"allowWindowsHostExecUac", config_->allow_windows_host_exec_uac}});
}
Json Engine::lifecycle(std::string name, const Cancel& cancel) {
    const auto verb = name.substr(7);
    const auto action = verb == "start"     ? LifecycleAction::start
                        : verb == "stop"    ? LifecycleAction::stop
                        : verb == "restart" ? LifecycleAction::restart
                                            : LifecycleAction::recreate;
    try {
        lifecycle_.set_guardian_desired_state(action != LifecycleAction::stop, "src/server.js:" + name);
        const auto data = lifecycle_.control(action, cancel);
        std::string summary;
        if (config_->runtime_mode == RuntimeMode::docker) {
            const auto state = action == LifecycleAction::start     ? " is running."
                               : action == LifecycleAction::stop    ? " is stopped."
                               : action == LifecycleAction::restart ? " has been restarted."
                                                                    : " has been recreated.";
            summary = "Docker Devbox " + json_string(data, "name", "devbox") + state;
        } else if (action == LifecycleAction::start)
            summary = config_->platform.display_name + " Host Devbox is ready in the current server process.";
        else
            summary = json_string(data, "controlMessage",
                                  config_->platform.display_name + " Host Devbox " + verb +
                                      " is managed by the launcher command.");
        return result_success(summary, Json(data));
    } catch (const std::exception& e) {
        return result_error("Failed to " + verb + " the " + config_->runtime_label() + ": " + e.what());
    }
}
asio::awaitable<Json> Engine::metadata(const HttpRequest& request) {
    const auto local = request.is_local ? "http://" + json_string(request.headers, "host") : "";
    const auto base = config_->public_base_url.value_or(local);
    Json value{{"name", config_->server_name()},
               {"version", build_version()},
               {"build", build_snapshot()},
               {"auth_mode", config_->auth_name()},
               {"runtime_mode", config_->runtime_name()},
               {"platform", config_->platform.id},
               {"public_base_url", config_->public_base_url ? Json(*config_->public_base_url) : Json()},
               {"local_base_url", local.empty() ? Json() : Json(local)},
               {"mcp_url", base.empty() ? Json() : Json(base + "/mcp")},
               {"root_mcp_url", base.empty() ? Json() : Json(base)},
               {"oauth", nullptr}};
    value["notes"] = config_->auth_mode == AuthMode::cloudflare_access
                         ? "Cloudflare Access-backed OAuth is enabled for ChatGPT app testing. Protect the "
                           "/authorize path with a Cloudflare Access application."
                         : std::string(config_->auth_mode == AuthMode::demo_oauth
                                           ? "Demo OAuth is enabled for ChatGPT app testing. "
                                           : "No authentication mode is active. ") +
                               "The " + config_->runtime_label() +
                               " is the main execution environment; host tools are separate and explicit.";
    if (config_->auth_mode != AuthMode::none && !request.is_local)
        co_return value;
    value["runtime"] = Json{{"runtimeMode", config_->runtime_name()},
                            {"platform", config_->platform.id},
                            {"hostShell", config_->host_shell},
                            {"devboxContainerName", config_->devbox_container_name},
                            {"devboxImageName", config_->devbox_image_name},
                            {"devboxWorkspacePath", path_text(config_->devbox_workspace_path)},
                            {"hostWorkspacePath", path_text(config_->host_workspace_path)},
                            {"hostExecEnabled", config_->host_exec_enabled}};
    auto pending = controls_.run(
        [this, cancel = request.cancellation] {
            try {
                return lifecycle_.status(cancel);
            } catch (const std::exception& e) {
                return Json{
                    {"exists", false}, {"running", false}, {"status", std::string("error: ") + e.what()}};
            }
        },
        request.cancellation);
    value["devbox"] = co_await std::move(pending);
    co_return value;
}
} // namespace devbox
