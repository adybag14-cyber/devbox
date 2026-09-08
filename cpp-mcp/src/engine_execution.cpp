#include "devbox/engine.hpp"
#include "devbox/result.hpp"
#include <algorithm>
namespace devbox {
namespace {
Json execution_data(const ExecutionLease& lease) {
    return Json{{"queue_wait_ms", lease.queue_wait_ms},
                {"slot", lease.slots.empty() ? Json() : Json(lease.slots.front())}};
}
std::pair<std::string, bool> trim_error(std::string_view text, std::size_t maximum) {
    if (js_length(text) <= maximum)
        return {std::string(text), false};
    const auto suffix = "\n... truncated to " + std::to_string(maximum) + " characters ...";
    const auto keep = maximum > suffix.size() ? maximum - suffix.size() : 0;
    return {js_slice(text, 0, keep) + suffix, true};
}
Json render_output(std::string summary, const ProcessOutput& output, const Json& args,
                   const ExecutionLease& lease, std::size_t limit) {
    const auto mode = json_string(args, "output_mode", "tail");
    const auto maximum = static_cast<std::size_t>(json_uint(args, "max_output_chars", limit));
    const auto lines = static_cast<std::size_t>(json_uint(args, "max_output_lines"));
    const auto out = shape_output(output.stdout_text, mode, maximum, lines),
               err = shape_output(output.stderr_text, mode, maximum, lines);
    return result_process(std::move(summary),
                          Json{{"execution", execution_data(lease)},
                               {"output",
                                {{"mode", mode},
                                 {"max_chars", maximum},
                                 {"max_lines", lines},
                                 {"stdout_original_chars", output.stdout_original_chars},
                                 {"stderr_original_chars", output.stderr_original_chars},
                                 {"stdout_capture_truncated", output.stdout_capture_truncated},
                                 {"stderr_capture_truncated", output.stderr_capture_truncated}}}},
                          out.text, err.text, output.exit_code, true,
                          out.truncated || err.truncated || output.stdout_capture_truncated ||
                              output.stderr_capture_truncated);
}
} // namespace
Json render_process_error(const std::exception& error, std::size_t maximum, std::optional<Json> data) {
    if (const auto* process = dynamic_cast<const ProcessError*>(&error)) {
        const auto out = trim_error(process->stdout_text, maximum),
                   err = trim_error(process->stderr_text, maximum);
        return result_process(error.what(), std::move(data), out.first, err.first, process->exit_code, false,
                              out.second || err.second);
    }
    if (dynamic_cast<const ElevationRequired*>(&error)) {
        if (!data)
            data = Json::object();
        (*data)["bridge_diagnostics"] = Json{
            {"suspected_elevation_gap", true},
            {"windows_host_exec_defaults_to_admin", true},
            {"allow_windows_host_exec_uac", false},
            {"hints",
             {"Keep MCP started only by elevated Guardian / ChatGptDevboxMcp-ElevatedStart (RunLevel "
              "Highest).",
              "Do not start MCP from a normal (non-admin) terminal if you want silent elevated host_exec.",
              "Set ALLOW_WINDOWS_HOST_EXEC_UAC=true only if you intentionally want per-command UAC "
              "prompts."}}};
        return result_process(error.what(), std::move(data), "", "", 740, false);
    }
    return result_process(error.what(), std::move(data), "", "", std::nullopt, false);
}
Json render_file_output(std::string summary, const ProcessOutput& output, std::size_t maximum) {
    const auto out = shape_output(output.stdout_text, "tail", maximum),
               err = shape_output(output.stderr_text, "tail", maximum);
    return result_process(std::move(summary),
                          Json{{"output",
                                {{"mode", "tail"},
                                 {"max_chars", maximum},
                                 {"max_lines", 0},
                                 {"stdout_original_chars", out.original_chars},
                                 {"stderr_original_chars", err.original_chars}}}},
                          out.text, err.text, output.exit_code, true, out.truncated || err.truncated);
}
asio::awaitable<ExecutionLease> Engine::acquire(AcquireRequest request, Cancel cancel) {
    auto pending = controls_.run(
        [this, request = std::move(request)] {
            return std::make_shared<ExecutionWaiter>(scheduler_.begin(request));
        },
        cancel);
    auto waiter = co_await std::move(pending);
    for (;;) {
        auto attempt = controls_.run([waiter, cancel] { return waiter->poll(cancel); }, cancel);
        auto lease = co_await std::move(attempt);
        if (lease)
            co_return std::move(*lease);
        co_await async_delay(waiter->poll_interval(), cancel);
    }
}
asio::awaitable<Json> Engine::execute(std::string name, Json args, Cancel cancel) {
    const bool host = name.starts_with("host_") || name.starts_with("windows_host_");
    const bool shell = name.find("run_program") == name.npos;
    const bool read_only = name == "devbox_exec_readonly";
    if (host && !config_->host_exec_enabled)
        co_return result_error("Host execution is disabled.");
    const auto command = json_string(args, "command"), program = trim(json_string(args, "program"));
    if (trim(shell ? command : program).empty())
        co_return result_error(shell ? "command must not be empty" : "program must not be empty");
    auto klass =
        shell ? infer_shell_resource(command) : infer_program_resource(program, json_strings(args, "args"));
    if (klass == ResourceClass::watch)
        klass = ResourceClass::light;
    const auto operation = shell ? command : program + " " + join(json_strings(args, "args"), " ");
    if (auto refusal = monitoring_.reject_disk_work(klass, read_only, operation))
        co_return *refusal;
    AcquireRequest request;
    request.resource_class = klass;
    request.weight = klass == ResourceClass::heavy      ? config_->exec_heavy_weight
                     : klass == ResourceClass::io_heavy ? config_->exec_io_heavy_weight
                                                        : 1;
    request.label = host ? (shell ? "host_exec" : "host_run_program:" + program)
                         : (shell ? name : "devbox_run_program:" + program);
    request.queue_timeout = Millis(config_->exec_queue_timeout_ms);
    auto lease = co_await acquire(std::move(request), cancel);
    ProcessOutput output;
    std::exception_ptr failure;
    const auto dir = working_dir(args, host);
    try {
        auto pending = commands_.run(
            [this, args, shell, host, dir, command, program, cancel] {
                const auto seconds = json_uint(args, "timeout_seconds", 90) + (host ? 5 : 0);
                const auto capture =
                    config_->command_output_limit_chars > std::numeric_limits<std::size_t>::max() / 2
                        ? std::numeric_limits<std::size_t>::max()
                        : config_->command_output_limit_chars * 2;
                if (shell) {
                    ShellRequest run;
                    run.command = command;
                    run.working_dir = dir;
                    run.timeout = Millis(seconds * 1000);
                    run.user = host ? "" : json_string(args, "user", config_->devbox_default_user);
                    run.max_capture_chars = capture;
                    return host ? runtime_.run_host_shell_only(run, cancel) : runtime_.run_shell(run, cancel);
                }
                ProgramRequest run;
                run.program = program;
                run.args = json_strings(args, "args");
                run.working_dir = dir;
                run.timeout = Millis(seconds * 1000);
                run.user = host ? "" : json_string(args, "user", config_->devbox_default_user);
                run.max_capture_chars = capture;
                return host ? runtime_.run_host_program_only(run, cancel) : runtime_.run_program(run, cancel);
            },
            cancel);
        output = co_await std::move(pending);
    } catch (...) {
        failure = std::current_exception();
    }
    auto release = controls_.run([&lease] { lease.release(); });
    co_await std::move(release);
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& e) {
            co_return render_process_error(e, config_->command_output_limit_chars,
                                           Json{{"execution", execution_data(lease)}});
        }
    }
    std::string summary;
    if (host && shell)
        summary = "Ran a " +
                  lower(config_->platform.is_windows ? "Windows PowerShell"
                                                     : config_->platform.display_name + " Host Shell") +
                  " command in " + path_text(dir) + ".";
    else if (host)
        summary = "Ran " + json_string(args, "program") + " on the " +
                  lower(config_->platform.display_name + " Host") + ".";
    else if (shell)
        summary =
            std::string(read_only ? "Ran a read-only shell command in the " : "Ran a shell command in the ") +
            config_->runtime_label() + " at " + path_text(dir) + ".";
    else
        summary =
            "Ran " + json_string(args, "program") + " directly in the " + config_->runtime_label() + ".";
    co_return render_output(summary, output, args, lease, config_->command_output_limit_chars);
}
asio::awaitable<Json> Engine::search(Json args, Cancel cancel) {
    SearchRequest request;
    request.pattern = json_string(args, "pattern");
    request.path = json_string(args, "path", path_text(config_->devbox_workspace_path));
    if (trim(request.path).empty())
        request.path = path_text(config_->devbox_workspace_path);
    request.glob = json_string(args, "glob", "*");
    request.case_sensitive = json_bool(args, "case_sensitive");
    request.max_matches = json_uint(args, "max_matches", 200);
    request.max_depth = json_uint(args, "max_depth", 12);
    request.max_file_bytes = json_uint(args, "max_file_bytes", 2097152);
    request.timeout = Millis(json_uint(args, "timeout_seconds", 30) * 1000);
    request.exclude_directories = json_strings(args, "exclude_directories");
    request.include_ignored = json_bool(args, "include_ignored");
    AcquireRequest admission;
    admission.label = "devbox_search_files";
    admission.resource_class = ResourceClass::io_heavy;
    admission.weight = config_->exec_io_heavy_weight;
    admission.queue_timeout = Millis(config_->exec_queue_timeout_ms);
    auto lease = co_await acquire(std::move(admission), cancel);
    ProcessOutput output;
    std::exception_ptr failure;
    try {
        auto pending =
            commands_.run([this, request, cancel] { return search_.search(request, cancel); }, cancel);
        output = co_await std::move(pending);
    } catch (...) {
        failure = std::current_exception();
    }
    auto release = controls_.run([&lease] { lease.release(); });
    co_await std::move(release);
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& e) {
            co_return result_error("Failed to search " + request.path + " inside the " +
                                       config_->runtime_label() + ": " + e.what(),
                                   Json{{"execution", execution_data(lease)}});
        }
    }
    const auto maximum = std::clamp<std::size_t>(config_->max_mcp_transfer_chars, 100, 65536);
    auto response = render_file_output("Searched " + request.path + " for \"" + request.pattern +
                                           "\" inside the " + config_->runtime_label() + ".",
                                       output, maximum);
    Json data{{"execution", execution_data(lease)},
              {"output", response["structuredContent"]["data"]["output"]}};
    const auto& envelope = response["structuredContent"];
    co_return result_process(json_string(envelope, "summary"), data, json_string(envelope, "stdout"),
                             json_string(envelope, "stderr"), output.exit_code, true,
                             json_bool(envelope, "truncated"));
}
} // namespace devbox
