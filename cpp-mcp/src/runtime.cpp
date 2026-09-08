#include "devbox/scoped_thread.hpp"
#include "devbox/runtime.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <future>
#include <regex>
#include <thread>
#ifdef _WIN32
#include <shellapi.h>
#endif
namespace devbox {
namespace {
constexpr auto disabled_message = "Windows host execution is disabled.";
constexpr auto elevation_message =
    "Windows host PowerShell requires the Devbox MCP process to already be elevated. This MCP process is "
    "medium-integrity, so host_exec refused to call Start-Process -Verb RunAs (that would spam UAC). "
    "Guardian treats unelevated MCP as unhealthy and restarts it via the Highest scheduled-task path. Retry "
    "after repair.";
std::string quiet_powershell(std::string_view command) {
    return "$ProgressPreference = 'SilentlyContinue'\n$InformationPreference = 'SilentlyContinue'\n" +
           std::string(command);
}
std::vector<std::string> file_powershell_args(const fs::path& path) {
    return {"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy",
            "Bypass",  "-File",      path_text(path)};
}
ProcessOptions process_options(const ProgramRequest& request) {
    ProcessOptions options;
    options.cwd = request.working_dir;
    options.timeout = request.timeout;
    options.max_capture_chars = request.max_capture_chars;
    options.input = request.input;
    options.on_output = request.on_output;
    options.on_pid = request.on_pid;
    return options;
}
ProcessOptions process_options(const ShellRequest& request) {
    ProcessOptions options;
    options.cwd = request.working_dir;
    options.timeout = request.timeout;
    options.max_capture_chars = request.max_capture_chars;
    options.on_output = request.on_output;
    options.on_pid = request.on_pid;
    return options;
}
bool launch_missing(const ProcessError& error) {
    if (error.exit_code || error.aborted || error.timed_out)
        return false;
    const auto message = lower(error.what());
    return message.find("os error 2") != std::string::npos ||
           message.find("no such file or directory") != std::string::npos ||
           message.find("cannot find the file") != std::string::npos ||
           message.find("program not found") != std::string::npos;
}
std::pair<std::string, std::vector<std::string>>
resolve_program(const Config& config, std::string_view program, const std::vector<std::string>& args) {
    if (lower(std::string(program)) == "node")
        return {config.node_exe, args};
#ifdef _WIN32
    const auto resolved = find_program(program);
    const auto path = resolved.value_or(path_from_utf8(program));
    if (lower(path_text(path.extension())) == ".ps1") {
        auto wrapped = file_powershell_args(path);
        wrapped.insert(wrapped.end(), args.begin(), args.end());
        return {config.power_shell_exe, wrapped};
    }
    return {path_text(path), args};
#else
    return {std::string(program), args};
#endif
}
ProcessOutput run_resolved(std::string program, const std::vector<std::string>& args, ProcessOptions options,
                           const Cancel& cancel) {
#ifdef _WIN32
    const auto extension = lower(path_text(path_from_utf8(program).extension()));
    if (extension == ".bat" || extension == ".cmd") {
        if (program.find('"') != std::string::npos || program.ends_with('\\'))
            throw Error("Windows file names may not contain quotes or end with a backslash");
        if (program.starts_with("\\\\?\\UNC\\"))
            program = "\\\\" + program.substr(8);
        else if (program.starts_with("\\\\?\\"))
            program.erase(0, 4);
        std::string raw = "/e:ON /v:OFF /d /c \"\"" + program + '"';
        for (const auto& arg : args)
            raw += " " + quote_batch_argument(arg);
        raw += '"';
        options.windows_raw_arguments = raw;
        const auto cmd = env_or("COMSPEC", "cmd.exe");
        return spawn_process(cmd, {}, options, cancel);
    }
#endif
    return spawn_process(program, args, options, cancel);
}
std::string decode_clixml(std::string_view value) {
    std::string output;
    for (std::size_t i = 0; i < value.size();) {
        if (i + 7 <= value.size() && value[i] == '_' && (value[i + 1] == 'x' || value[i + 1] == 'X') &&
            value[i + 6] == '_') {
            try {
                const auto digits = std::string(value.substr(i + 2, 4));
                std::size_t consumed = 0;
                const auto point = std::stoul(digits, &consumed, 16);
                if (consumed == 4 && !(point >= 0xd800 && point <= 0xdfff)) {
                    output += from_utf16(std::u16string(1, static_cast<char16_t>(point)));
                    i += 7;
                    continue;
                }
            } catch (...) {
            }
        }
        output += value[i++];
    }
    for (const auto& pair :
         {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}})
        output = replace_all(std::move(output), pair.first, pair.second);
    return output;
}
ProcessError cleaned_error(const ProcessError& original) {
    auto message = trim(clean_powershell_output(original.what()));
    ProcessError result(message.empty() ? "Windows PowerShell command failed." : message);
    result.exit_code = original.exit_code;
    result.signal = original.signal;
    result.stdout_text = clean_powershell_output(original.stdout_text);
    result.stderr_text = clean_powershell_output(original.stderr_text);
    result.file = original.file;
    result.args = original.args;
    result.aborted = original.aborted;
    result.timed_out = original.timed_out;
    result.elapsed_ms = original.elapsed_ms;
    return result;
}
} // namespace
ProcessOutput run_native_program(std::string_view program, const std::vector<std::string>& args,
                                 ProcessOptions options, const Cancel& cancel) {
#ifdef _WIN32
    const auto resolved = find_program(program, options.env ? &*options.env : nullptr);
    const auto path = resolved ? path_text(*resolved) : std::string(program);
#else
    const auto path = std::string(program);
#endif
    return run_resolved(path, args, std::move(options), cancel);
}
std::string quote_batch_argument(std::string_view argument) {
    if (argument.find_first_of("\r\n") != std::string_view::npos ||
        argument.find('\0') != std::string_view::npos)
        throw Error("batch file arguments are invalid");
    const std::string_view safe = "#$*+-./:?@\\_";
    bool quote = argument.empty() || argument.ends_with('\\');
    for (const auto c : argument) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80 && !((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
                             (byte >= 'A' && byte <= 'Z') || safe.find(c) != std::string_view::npos))
            quote = true;
    }
    std::string out = quote ? "\"" : "";
    std::size_t slashes = 0;
    for (const auto c : argument) {
        if (c == '\\')
            ++slashes;
        else {
            if (c == '"') {
                out.append(slashes, '\\');
                out += '"';
            }
            if (c == '%')
                out += "%%cd:~,";
            slashes = 0;
        }
        out += c;
    }
    if (quote) {
        out.append(slashes, '\\');
        out += '"';
    }
    return out;
}
std::vector<std::string> encoded_powershell_args(std::string_view command) {
    const auto text = to_utf16(quiet_powershell(command));
    std::string bytes;
    bytes.reserve(text.size() * 2);
    for (const auto unit : text) {
        bytes += static_cast<char>(unit & 0xff);
        bytes += static_cast<char>(unit >> 8);
    }
    return {"-NoLogo", "-NoProfile",      "-NonInteractive",   "-ExecutionPolicy",
            "Bypass",  "-EncodedCommand", base64_encode(bytes)};
}
std::string clean_powershell_output(std::string_view value) {
    if (value.find("#< CLIXML") == std::string_view::npos)
        return std::string(value);
    const std::regex channel(
        R"xml(<S\s+S="(?:Error|Warning|Verbose|Debug|Information|Output)"[^>]*>([\s\S]*?)</S>)xml",
        std::regex::icase);
    std::string result;
    std::size_t position = 0;
    while (position < value.size()) {
        const auto start = value.find("#< CLIXML", position);
        if (start == std::string_view::npos) {
            result += value.substr(position);
            break;
        }
        const auto end = value.find("</Objs>", start);
        if (end == std::string_view::npos) {
            result += value.substr(position);
            break;
        }
        auto object = start + std::string_view("#< CLIXML").size();
        while (object < value.size() && std::isspace(static_cast<unsigned char>(value[object])))
            ++object;
        if (value.substr(object, 5) != "<Objs") {
            result += value.substr(position, start + 1 - position);
            position = start + 1;
            continue;
        }
        result += value.substr(position, start - position);
        const auto envelope = std::string(value.substr(start, end + 7 - start));
        for (auto match = std::sregex_iterator(envelope.begin(), envelope.end(), channel);
             match != std::sregex_iterator(); ++match) {
            const auto text = trim(decode_clixml((*match)[1].str()));
            if (!text.empty())
                result += text + '\n';
        }
        position = end + 7;
    }
    return result;
}
ProcessOutput RuntimeExecutor::host_program(ProgramRequest request, const Cancel& cancel) const {
    if (!config_->host_exec_enabled)
        throw Error(disabled_message);
    const auto [program, args] = resolve_program(*config_, request.program, request.args);
    return run_resolved(program, args, process_options(request), cancel);
}
ProcessOutput RuntimeExecutor::run_program(ProgramRequest request, const Cancel& cancel) const {
    const auto normalized = normalize_program(request.program);
    if (normalized.empty() ||
        std::find(config_->devbox_program_allowlist.begin(), config_->devbox_program_allowlist.end(),
                  normalized) == config_->devbox_program_allowlist.end())
        throw Error("Program \"" + request.program + "\" is not in DEVBOX_PROGRAM_ALLOWLIST: " +
                    join(config_->devbox_program_allowlist, ", "));
    request.program = normalized;
    if (config_->runtime_mode == RuntimeMode::host)
        return host_program(std::move(request), cancel);
    std::vector<std::string> args{"exec"};
    if (request.input)
        args.push_back("-i");
    if (!trim(request.user).empty()) {
        args.push_back("-u");
        args.push_back(request.user);
    }
    args.insert(args.end(),
                {"-w", path_text(request.working_dir), config_->devbox_container_name, request.program});
    args.insert(args.end(), request.args.begin(), request.args.end());
    auto options = process_options(request);
    options.cwd.reset();
    return spawn_process("docker", args, options, cancel);
}
ProcessOutput RuntimeExecutor::run_host_program_only(ProgramRequest request, const Cancel& cancel) const {
    const auto normalized = normalize_program(request.program);
    if (normalized.empty() ||
        std::find(config_->host_program_allowlist.begin(), config_->host_program_allowlist.end(),
                  normalized) == config_->host_program_allowlist.end())
        throw Error("Program \"" + request.program +
                    "\" is not in HOST_PROGRAM_ALLOWLIST: " + join(config_->host_program_allowlist, ", "));
    request.program = normalized;
    return host_program(std::move(request), cancel);
}
ProcessOutput RuntimeExecutor::powershell(const ShellRequest& request, const Cancel& cancel,
                                          bool clean) const {
    auto args = encoded_powershell_args(request.command);
    std::optional<fs::path> directory;
    ScopeExit cleanup([&] {
        if (directory) {
            std::error_code ec;
            fs::remove_all(*directory, ec);
        }
    });
    if (join(args, " ").size() >= 24000) {
        directory = fs::temp_directory_path() / path_from_utf8("devbox-cpp-powershell-" + uuid());
        fs::create_directory(*directory);
        const auto script = *directory / "command.ps1";
        write_file(script, "\xef\xbb\xbf" + quiet_powershell(request.command));
        args = file_powershell_args(script);
    }
    std::vector<std::string> candidates{config_->power_shell_exe};
    if (!config_->power_shell_fallback_exe.empty() && config_->power_shell_fallback_exe != candidates.front())
        candidates.push_back(config_->power_shell_fallback_exe);
    const auto started = Clock::now();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto options = process_options(request);
        options.timeout =
            std::max(Millis(1), request.timeout - std::chrono::duration_cast<Millis>(Clock::now() - started));
        try {
            auto output = spawn_process(candidates[i], args, options, cancel);
            if (clean) {
                output.stdout_text = clean_powershell_output(output.stdout_text);
                output.stderr_text = clean_powershell_output(output.stderr_text);
            }
            return output;
        } catch (const ProcessError& error) {
            if (i + 1 < candidates.size() && launch_missing(error))
                continue;
            if (clean)
                throw cleaned_error(error);
            throw;
        }
    }
    throw Error("No usable PowerShell executable is configured.");
}
ProcessOutput RuntimeExecutor::host_runtime_shell(const ShellRequest& request, const Cancel& cancel) const {
    if (!config_->host_exec_enabled)
        throw Error(disabled_message);
    const auto name = normalize_program(config_->host_shell);
#ifdef _WIN32
    if (name == "powershell" || name == "pwsh") {
        // A HOST_SHELL override must remain authoritative even when POWERSHELL_EXE differs.
        auto override_config = std::make_shared<Config>(*config_);
        override_config->power_shell_exe = config_->host_shell;
        if (config_->host_shell != config_->power_shell_exe)
            override_config->power_shell_fallback_exe.clear();
        return RuntimeExecutor(override_config).powershell(request, cancel, false);
    }
    if (name == "cmd") {
        if (js_length(request.command) > 8000)
            throw Error("CMD inline commands are limited to 8000 UTF-16 units; save a .cmd script and run "
                        "that file instead.");
        auto options = process_options(request);
        options.windows_raw_arguments = "/d /s /c \"" + request.command + '"';
        return spawn_process(config_->host_shell, {}, options, cancel);
    }
#endif
    return spawn_process(config_->host_shell, {"-lc", request.command}, process_options(request), cancel);
}
ProcessOutput RuntimeExecutor::run_shell(const ShellRequest& request, const Cancel& cancel) const {
    if (config_->runtime_mode == RuntimeMode::host)
        return host_runtime_shell(request, cancel);
    std::vector<std::string> args{"exec"};
    if (!trim(request.user).empty()) {
        args.push_back("-u");
        args.push_back(request.user);
    }
    args.insert(args.end(), {"-w", path_text(request.working_dir), config_->devbox_container_name, "bash",
                             "-lc", request.command});
    auto options = process_options(request);
    options.cwd.reset();
    return spawn_process("docker", args, options, cancel);
}
ProcessOutput RuntimeExecutor::run_host_shell_only(const ShellRequest& request, const Cancel& cancel) const {
    if (!config_->host_exec_enabled)
        throw Error(disabled_message);
#ifdef _WIN32
    if (!is_administrator()) {
        if (!config_->allow_windows_host_exec_uac)
            throw ElevationRequired(elevation_message);
        return elevated_shell(request, cancel);
    }
    return powershell(request, cancel, true);
#else
    return host_runtime_shell(request, cancel);
#endif
}
ProcessOutput RuntimeExecutor::run_inspection_shell(const ShellRequest& request, const Cancel& cancel) const {
    if (!config_->host_exec_enabled)
        throw Error(disabled_message);
#ifdef _WIN32
    return powershell(request, cancel, true);
#else
    return host_runtime_shell(request, cancel);
#endif
}
std::optional<std::vector<std::string>> RuntimeExecutor::cached_versions() const {
    std::lock_guard lock(versions_mutex_);
    return Clock::now() < versions_expiry_ ? versions_ : std::nullopt;
}
std::vector<std::string> RuntimeExecutor::get_versions(bool force, const Cancel& cancel) {
    if (!force)
        if (auto cached = cached_versions())
            return *cached;
    std::vector<std::string> versions;
    if (config_->runtime_mode == RuntimeMode::docker) {
        const std::string command =
            "printf 'gh='; if command -v gh >/dev/null 2>&1; then printf 'installed\\n'; else printf "
            "'missing\\n'; fi && printf 'node='; node --version && printf 'npm='; if command -v npm "
            ">/dev/null 2>&1; then printf 'installed\\n'; else printf 'missing\\n'; fi && printf 'python='; "
            "python3 --version && printf 'git='; git --version && printf 'rg='; rg --version | head -n 1";
        ProcessOptions options;
        options.timeout = Millis(20000);
        options.max_capture_chars = 32768;
        auto output = spawn_process("docker",
                                    {"exec", "-w", path_text(config_->devbox_workspace_path),
                                     config_->devbox_container_name, "bash", "-lc", command},
                                    options, cancel);
        for (auto line : split(output.stdout_text, '\n', false))
            if (!trim(line).empty())
                versions.push_back(trim(line));
    } else {
        const auto programs =
            config_->platform.is_windows
                ? std::vector<std::string>{"node", "npm", "git", "gh", "python", "pwsh", "rg", "curl"}
                : std::vector<std::string>{"node", "npm", "git", "gh", "python3", "rg"};
        versions.resize(programs.size());
        std::atomic_size_t next{0};
        const auto probe = [&] {
            while (true) {
                const auto i = next.fetch_add(1);
                if (i >= programs.size())
                    break;
                const auto& name = programs[i];
                try {
                    const auto [program, args] = resolve_program(*config_, name, {"--version"});
                    ProcessOptions options;
                    options.cwd = config_->host_default_workdir;
                    options.timeout = Millis(15000);
                    options.max_capture_chars = 16384;
                    const auto output = run_resolved(program, args, options, cancel);
                    std::string value = "available";
                    for (const auto& line : split(output.stdout_text + output.stderr_text, '\n', false))
                        if (!trim(line).empty()) {
                            value = trim(line);
                            break;
                        }
                    versions[i] = name + "=" + value;
                } catch (...) {
                    versions[i] = name + "=unavailable";
                }
            }
        };
        auto other = std::async(std::launch::async, probe);
        probe();
        other.get();
    }
    std::lock_guard lock(versions_mutex_);
    versions_ = versions;
    versions_expiry_ = Clock::now() + Millis(config_->devbox_version_cache_ms);
    return versions;
}
ProcessOutput RuntimeExecutor::elevated_shell(const ShellRequest& request, const Cancel& cancel) const {
#ifdef _WIN32
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-elevated-" + uuid());
    fs::create_directory(root);
    bool finished = false;
    ScopeExit cleanup([&] {
        if (finished) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    });
    const auto request_path = root / "request.json";
    const auto timeout = static_cast<std::uint64_t>(std::max<Millis::rep>(1, request.timeout.count()));
    write_json_atomic(request_path,
                      Json{{"schema_version", 1},
                           {"command", request.command},
                           {"working_dir", path_text(request.working_dir)},
                           {"timeout_ms", timeout},
                           {"max_capture_chars",
                            request.max_capture_chars ? Json(*request.max_capture_chars) : Json(nullptr)},
                           {"powershell", config_->power_shell_exe},
                           {"fallback", config_->power_shell_fallback_exe}});
    const auto binary = executable_path().wstring();
    const auto parameters =
        wide("--elevated-shell-worker " + quote_windows_argument(path_text(request_path)));
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"runas";
    launch.lpFile = binary.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_HIDE;
    if (cancel)
        cancel->check();
    if (!ShellExecuteExW(&launch)) {
        finished = true;
        throw Error(windows_error());
    }
    NativeHandle worker(launch.hProcess);
    if (!worker)
        throw Error("The elevated C++ worker did not return a process handle.");
    if (request.on_pid)
        request.on_pid(GetProcessId(worker.get()));
    const auto started = Clock::now();
    bool cancelled = false, timed_out = false;
    std::optional<Clock::time_point> stopped;
    while (WaitForSingleObject(worker.get(), 10) == WAIT_TIMEOUT) {
        const auto now = Clock::now();
        if (!stopped &&
            ((cancel && cancel->cancelled()) || now - started >= request.timeout + Millis(15000))) {
            cancelled = cancel && cancel->cancelled();
            timed_out = !cancelled;
            write_json_atomic(root / "cancel.json", Json{{"cancel", true}});
            stopped = now;
        }
        if (stopped && now - *stopped > Millis(15000))
            throw Error("Elevated worker cancellation remains pending; its request directory was retained "
                        "for recovery: " +
                        path_text(root));
    }
    finished = true;
    const auto data = read_json(
        root / "result.json",
        std::max<std::size_t>(16 * 1024 * 1024, request.max_capture_chars.value_or(4 * 1024 * 1024) * 8));
    ProcessOutput output;
    output.stdout_text = json_string(data, "stdout");
    output.stderr_text = json_string(data, "stderr");
    output.stdout_original_chars = static_cast<std::size_t>(json_uint(data, "stdout_original_chars"));
    output.stderr_original_chars = static_cast<std::size_t>(json_uint(data, "stderr_original_chars"));
    output.stdout_capture_truncated = json_bool(data, "stdout_capture_truncated");
    output.stderr_capture_truncated = json_bool(data, "stderr_capture_truncated");
    output.pid = static_cast<std::uint32_t>(json_uint(data, "pid"));
    output.elapsed_ms = json_uint(data, "elapsed_ms");
    if (request.on_output) {
        if (!output.stdout_text.empty())
            request.on_output(OutputStream::stdout_stream, output.stdout_text);
        if (!output.stderr_text.empty())
            request.on_output(OutputStream::stderr_stream, output.stderr_text);
    }
    if (!json_bool(data, "ok") || cancelled || timed_out) {
        ProcessError error(cancelled   ? "Command cancelled by the MCP client."
                           : timed_out ? "Elevated command timed out."
                                       : json_string(data, "message", "Windows PowerShell command failed."));
        if (data.contains("exit_code") && data["exit_code"].is_number_integer())
            error.exit_code = data["exit_code"].get<int>();
        error.stdout_text = output.stdout_text;
        error.stderr_text = output.stderr_text;
        error.file = config_->power_shell_exe;
        error.args = encoded_powershell_args(request.command);
        error.aborted = cancelled || json_bool(data, "aborted");
        error.timed_out = timed_out || json_bool(data, "timed_out");
        error.elapsed_ms = output.elapsed_ms;
        throw error;
    }
    output.exit_code = data["exit_code"].get<int>();
    return output;
#else
    (void)request;
    (void)cancel;
    throw Error("Windows elevation is unavailable on this platform.");
#endif
}
int elevated_shell_worker(const fs::path& request_path) {
#ifdef _WIN32
    if (!is_administrator())
        throw Error("Elevated worker requires an administrator token.");
    const auto request = read_json(request_path, 8 * 1024 * 1024);
    if (json_uint(request, "schema_version") != 1)
        throw Error("Unknown elevated worker request schema");
    auto config = std::make_shared<Config>();
    config->host_exec_enabled = true;
    config->runtime_mode = RuntimeMode::host;
    config->platform = Platform::detect();
    config->power_shell_exe = request["powershell"].get<std::string>();
    config->power_shell_fallback_exe = json_string(request, "fallback");
    ShellRequest shell;
    shell.command = request["command"].get<std::string>();
    shell.working_dir = path_from_utf8(request["working_dir"].get<std::string>());
    shell.timeout = Millis(std::min<std::uint64_t>(json_uint(request, "timeout_ms", 30000),
                                                   static_cast<std::uint64_t>(INT64_MAX)));
    if (request.contains("max_capture_chars") && !request["max_capture_chars"].is_null())
        shell.max_capture_chars = request["max_capture_chars"].get<std::size_t>();
    auto cancel = std::make_shared<Cancellation>();
    std::atomic_bool done{false};
    ScopedThread watcher([&] {
        while (!done && !cancel->wait_for(Millis(25))) {
            std::error_code ec;
            if (fs::exists(request_path.parent_path() / "cancel.json", ec))
                cancel->cancel();
        }
    });
    ScopeExit stop([&] {
        done = true;
        cancel->cancel();
    });
    Json result;
    try {
        const auto output = RuntimeExecutor(config).run_inspection_shell(shell, cancel);
        result = Json{{"ok", true},
                      {"stdout", output.stdout_text},
                      {"stderr", output.stderr_text},
                      {"exit_code", output.exit_code},
                      {"pid", output.pid},
                      {"elapsed_ms", output.elapsed_ms},
                      {"stdout_original_chars", output.stdout_original_chars},
                      {"stderr_original_chars", output.stderr_original_chars},
                      {"stdout_capture_truncated", output.stdout_capture_truncated},
                      {"stderr_capture_truncated", output.stderr_capture_truncated}};
    } catch (const ProcessError& error) {
        result = Json{{"ok", false},
                      {"stdout", error.stdout_text},
                      {"stderr", error.stderr_text},
                      {"message", error.what()},
                      {"exit_code", error.exit_code ? Json(*error.exit_code) : Json(nullptr)},
                      {"timed_out", error.timed_out},
                      {"aborted", error.aborted},
                      {"elapsed_ms", error.elapsed_ms}};
    } catch (const std::exception& error) {
        result = Json{{"ok", false}, {"message", error.what()}};
    }
    write_json_atomic(request_path.parent_path() / "result.json", result);
    return json_bool(result, "ok") ? 0 : 1;
#else
    (void)request_path;
    throw Error("Windows elevation is unavailable on this platform.");
#endif
}
} // namespace devbox
