#include "devbox/lifecycle.hpp"
namespace devbox {
ProcessOutput GithubAuthService::run(bool host, const std::string& program, std::vector<std::string> args,
                                     Millis timeout, const Cancel& cancel,
                                     std::optional<std::string> input) const {
    ProgramRequest request;
    request.program = program;
    request.args = std::move(args);
    request.timeout = timeout;
    request.input = std::move(input);
    request.max_capture_chars = 65536;
    request.working_dir = host || config_->runtime_mode == RuntimeMode::host ? config_->host_default_workdir
                                                                             : config_->devbox_workspace_path;
    request.user = host ? "" : config_->devbox_default_user;
    return host ? runtime_.run_host_program_only(std::move(request), cancel)
                : runtime_.run_program(std::move(request), cancel);
}
void GithubAuthService::ensure_selected(const Cancel& cancel) const {
    if (config_->runtime_mode == RuntimeMode::docker)
        lifecycle_.control(LifecycleAction::start, cancel);
}
std::string GithubAuthService::identity(bool host, const std::string& key, const Cancel& cancel) const {
    if (!host && config_->runtime_mode == RuntimeMode::docker) {
        ShellRequest request;
        request.command = "git config --global --get " + key + " || true";
        request.working_dir = config_->devbox_workspace_path;
        request.timeout = Millis(5000);
        request.user = config_->devbox_default_user;
        request.max_capture_chars = 8192;
        return trim(runtime_.run_shell(request, cancel).stdout_text);
    }
    try {
        return trim(run(true, "git", {"config", "--global", key}, Millis(5000), cancel).stdout_text);
    } catch (const ProcessError& e) {
        if (e.aborted)
            throw;
        return "";
    }
}
std::pair<std::string, Json> GithubAuthService::host_context(const Cancel& cancel) const {
    if (!config_->host_exec_enabled)
        throw Error("Host execution is disabled.");
    const auto status =
        run(true, "gh", {"auth", "status", "--hostname", "github.com"}, Millis(15000), cancel);
    const auto token = trim(
        run(true, "gh", {"auth", "token", "--hostname", "github.com"}, Millis(15000), cancel).stdout_text);
    if (token.empty())
        throw Error("Host GitHub CLI did not return a token.");
    return {token, Json{{"statusSummary", trim(status.stdout_text + status.stderr_text)},
                        {"userName", identity(true, "user.name", cancel)},
                        {"userEmail", identity(true, "user.email", cancel)}}};
}
Json GithubAuthService::selected_status(const Cancel& cancel) const {
    ensure_selected(cancel);
    const auto status =
        run(false, "gh", {"auth", "status", "--hostname", "github.com"}, Millis(15000), cancel);
    return Json{{"statusSummary", trim(status.stdout_text + status.stderr_text)},
                {"userName", identity(false, "user.name", cancel)},
                {"userEmail", identity(false, "user.email", cancel)}};
}
Json GithubAuthService::status(const Cancel& cancel) const {
    return config_->runtime_mode == RuntimeMode::host ? host_context(cancel).second : selected_status(cancel);
}
Json GithubAuthService::sync_from_host(const Cancel& cancel) const {
    auto [token, host] = host_context(cancel);
    ensure_selected(cancel);
    try {
        run(false, "gh", {"auth", "login", "--hostname", "github.com", "--with-token"}, Millis(20000), cancel,
            token + "\n");
    } catch (const ProcessError& e) {
        // External programs can echo stdin on failure. Never expose a token through diagnostics.
        ProcessError safe(replace_all(e.what(), token, "[redacted]"));
        safe.exit_code = e.exit_code;
        safe.signal = e.signal;
        safe.timed_out = e.timed_out;
        safe.aborted = e.aborted;
        safe.elapsed_ms = e.elapsed_ms;
        safe.file = e.file;
        safe.stdout_text = e.stdout_text;
        safe.stderr_text = e.stderr_text;
        safe.stdout_text = replace_all(safe.stdout_text, token, "[redacted]");
        safe.stderr_text = replace_all(safe.stderr_text, token, "[redacted]");
        safe.args.clear();
        throw safe;
    }
    run(false, "gh", {"auth", "setup-git", "--hostname", "github.com"}, Millis(15000), cancel);
    for (const auto& pair : {std::pair{"user.name", "userName"}, std::pair{"user.email", "userEmail"}}) {
        const auto value = json_string(host, pair.second);
        if (!value.empty())
            run(false, "git", {"config", "--global", pair.first, value}, Millis(5000), cancel);
    }
    return Json{{"status", selected_status(cancel)},
                {"hostUserName", host["userName"]},
                {"hostUserEmail", host["userEmail"]}};
}
} // namespace devbox
