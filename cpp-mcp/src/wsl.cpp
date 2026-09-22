#include "devbox/wsl.hpp"
#include "devbox/native.hpp"
#include <algorithm>
namespace devbox {
namespace {
void literal(std::string_view value, std::size_t maximum, bool required = true) {
    if ((required && value.empty()) || value.size() > maximum || value.find('\0') != value.npos ||
        value.find('\r') != value.npos || value.find('\n') != value.npos)
        throw Error("WSL_ARGUMENT_INVALID");
}
std::string distro(const Json& args) {
    const auto value = json_string(args, "distribution");
    literal(value, 256);
    if (value.starts_with('-'))
        throw Error("WSL_DISTRIBUTION_INVALID");
    return value;
}
std::string linux_path(const Json& args, const char* key) {
    const auto value = json_string(args, key);
    literal(value, 2048);
    if (!value.starts_with('/') || value.starts_with("//"))
        throw Error("WSL_ABSOLUTE_LINUX_PATH_REQUIRED");
    return value;
}
fs::path executable() {
#ifdef _WIN32
    return path_from_utf8(env_or("SystemRoot", "C:\\Windows")) / "System32" / "wsl.exe";
#else
    return {};
#endif
}
std::string console_text(const std::string& raw) {
    if (raw.find('\0') == std::string::npos)
        return trim(sanitize_utf8(raw));
    if (raw.size() % 2)
        throw Error("WSL_INVALID_UTF16_RESPONSE");
    std::u16string units;
    for (std::size_t i = 0; i < raw.size(); i += 2)
        units.push_back(static_cast<char16_t>(static_cast<unsigned char>(raw[i]) |
                                              (static_cast<unsigned char>(raw[i + 1]) << 8)));
    if (!units.empty() && units.front() == 0xfeff)
        units.erase(units.begin());
    return trim(from_utf16(units));
}
ProcessOutput launch(const std::vector<std::string>& args, Millis timeout, const Cancel& cancel,
                     const std::optional<std::string>& input = {}, bool raw_capture = false) {
    ProcessOptions options;
    options.env = worker_environment();
    options.timeout = timeout;
    options.input = input;
    options.max_capture_chars = raw_capture ? 0 : 65536;
    options.termination_grace = Millis(1000);
    std::string raw;
    if (raw_capture)
        options.on_output = [&](OutputStream stream, std::string_view bytes) {
            if (stream == OutputStream::stdout_stream) {
                if (bytes.size() > 65536 - raw.size())
                    throw Error("WSL_INVENTORY_BUDGET");
                raw.append(bytes);
            }
        };
    auto result = spawn_process(path_text(executable()), args, options, cancel);
    if (raw_capture)
        result.stdout_text = console_text(raw);
    return result;
}
Json distributions(const Cancel& cancel) {
    const auto output = launch({"--list", "--quiet"}, Millis(10000), cancel, {}, true);
    Json names = Json::array();
    for (const auto& line : split(output.stdout_text, '\n', false)) {
        const auto name = trim(line);
        if (!name.empty())
            names.push_back(name);
        if (names.size() > 64)
            throw Error("WSL_DISTRIBUTION_BUDGET");
    }
    return names;
}
} // namespace
Json wsl_capabilities(const Config& config) {
    std::string status = !config.platform.is_windows          ? "unsupported"
                         : !config.host_exec_enabled          ? "permission_denied"
                         : !fs::is_regular_file(executable()) ? "unavailable"
                                                              : "available";
    return Json{{"status", status},
                {"authority", "trusted_operator"},
                {"distribution_required", true},
                {"linux_paths_explicit", true},
                {"argument_vector", true},
                {"shell_command_field", false},
                {"requires_guest_utilities", {"/usr/bin/env", "/usr/bin/timeout", "/usr/bin/wslpath"}},
                {"cancellation_proves_guest_termination", false},
                {"autonomous_sandbox", false},
                {"lifecycle_controls", "backend_does_not_issue_distribution_lifecycle_commands"}};
}
std::vector<std::string> wsl_program_arguments(const Json& args) {
    const auto distribution = distro(args), program = linux_path(args, "program"),
               workdir = linux_path(args, "working_dir");
    const auto seconds = json_uint(args, "timeout_seconds", 30);
    if (!seconds || seconds > 300)
        throw Error("WSL_DEADLINE_INVALID");
    const auto supplied = json_strings(args, "args");
    if (supplied.size() > 128)
        throw Error("WSL_ARGUMENT_BUDGET");
    std::vector<std::string> result{"--distribution",
                                    distribution,
                                    "--cd",
                                    workdir,
                                    "--exec",
                                    "/usr/bin/env",
                                    "-i",
                                    "PATH=/usr/bin:/bin",
                                    "LANG=C.UTF-8",
                                    "/usr/bin/timeout",
                                    "--signal=TERM",
                                    "--kill-after=1s",
                                    std::to_string(seconds) + "s",
                                    program};
    std::size_t bytes = 0;
    for (const auto& value : supplied) {
        if (value.size() > 8192 || value.find('\0') != value.npos)
            throw Error("WSL_ARGUMENT_INVALID");
        bytes += value.size();
        result.push_back(value);
    }
    if (bytes > 16000)
        throw Error("WSL_ARGUMENT_BUDGET");
    return result;
}
Json wsl_operation(const Config& config, const Json& args, const Cancel& cancel) {
    auto result = wsl_capabilities(config);
    if (result["status"] != "available")
        return result;
    const auto action = json_string(args, "action");
    if (action == "distributions") {
        result["distributions"] = distributions(cancel);
        return result;
    }
    const auto distribution = distro(args);
    const auto names = distributions(cancel);
    if (std::find(names.begin(), names.end(), Json(distribution)) == names.end())
        throw Error("WSL_DISTRIBUTION_NOT_REGISTERED");
    result["distribution"] = distribution;
    if (action == "map_path") {
        const auto path = json_string(args, "path"), direction = json_string(args, "direction", "to_linux");
        literal(path, 2048);
        if (direction != "to_linux" && direction != "to_windows")
            throw Error("WSL_MAPPING_DIRECTION_INVALID");
        const auto mapped =
            launch({"--distribution", distribution, "--exec", "/usr/bin/env", "-i", "PATH=/usr/bin:/bin",
                    "/usr/bin/wslpath", "-a", direction == "to_linux" ? "-u" : "-w", path},
                   Millis(10000), cancel);
        result["mapped_path"] = trim(mapped.stdout_text);
        result["direction"] = direction;
        return result;
    }
    if (action != "run")
        throw Error("WSL_ACTION_INVALID");
    const auto arguments = wsl_program_arguments(args);
    const auto uid =
        launch({"--distribution", distribution, "--exec", "/usr/bin/id", "-u"}, Millis(10000), cancel);
    if (trim(uid.stdout_text) == "0")
        throw Error(
            "WSL_ROOT_DEFAULT_USER_DENIED: select a distribution configured with a regular default user");
    const auto seconds = json_uint(args, "timeout_seconds", 30);
    std::optional<std::string> input;
    if (args.contains("input")) {
        input = json_string(args, "input");
        if (input->size() > 131072)
            throw Error("WSL_INPUT_BUDGET");
    }
    try {
        const auto output = launch(arguments, Millis(seconds * 1000 + 5000), cancel, input);
        result["status"] = "completed";
        result["exit_code"] = output.exit_code;
        result["stdout"] = output.stdout_text;
        result["stderr"] = output.stderr_text;
        result["stdout_truncated"] = output.stdout_capture_truncated;
        result["stderr_truncated"] = output.stderr_capture_truncated;
        result["elapsed_ms"] = output.elapsed_ms;
    } catch (const ProcessError& error) {
        result["status"] = error.aborted                                      ? "cancel_requested"
                           : error.timed_out                                  ? "bridge_deadline"
                           : error.exit_code == 124 || error.exit_code == 137 ? "guest_deadline"
                                                                              : "failed";
        result["exit_code"] = error.exit_code ? Json(*error.exit_code) : Json(nullptr);
        result["stdout"] = error.stdout_text;
        result["stderr"] = error.stderr_text;
        result["workload_termination_verified"] = false;
        result["detail"] = "The bridge reports cancellation/deadline receipt separately from guest workload "
                           "termination; it never shuts down a distribution";
    }
    return result;
}
} // namespace devbox
