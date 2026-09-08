#pragma once
#include "config.hpp"
#include "process.hpp"
namespace devbox {
struct ElevationRequired : Error {
    using Error::Error;
};
struct ProgramRequest {
    std::string program;
    std::vector<std::string> args;
    std::optional<std::string> input;
    fs::path working_dir;
    Millis timeout{30000};
    std::string user;
    std::optional<std::size_t> max_capture_chars;
    std::function<void(OutputStream, std::string_view)> on_output;
    std::function<void(std::uint32_t)> on_pid;
};
struct ShellRequest {
    std::string command;
    fs::path working_dir;
    Millis timeout{30000};
    std::string user;
    std::optional<std::size_t> max_capture_chars;
    std::function<void(OutputStream, std::string_view)> on_output;
    std::function<void(std::uint32_t)> on_pid;
};
std::string clean_powershell_output(std::string_view value);
std::vector<std::string> encoded_powershell_args(std::string_view command);
std::string quote_batch_argument(std::string_view argument);
// Native process launch plus the Windows .cmd/.bat executable-file wrapper.
// The installer shares the same tested quoting and process-tree ownership.
ProcessOutput run_native_program(std::string_view program, const std::vector<std::string>& args,
                                 ProcessOptions options, const Cancel& cancel = {});
class RuntimeExecutor {
    std::shared_ptr<const Config> config_;
    mutable std::mutex versions_mutex_;
    std::optional<std::vector<std::string>> versions_;
    Clock::time_point versions_expiry_{};
    ProcessOutput host_program(ProgramRequest request, const Cancel& cancel) const;
    ProcessOutput powershell(const ShellRequest& request, const Cancel& cancel, bool clean) const;
    ProcessOutput host_runtime_shell(const ShellRequest& request, const Cancel& cancel) const;
    ProcessOutput elevated_shell(const ShellRequest& request, const Cancel& cancel) const;

  public:
    explicit RuntimeExecutor(std::shared_ptr<const Config> config) : config_(std::move(config)) {}
    const Config& config() const {
        return *config_;
    }
    ProcessOutput run_program(ProgramRequest request, const Cancel& cancel = {}) const;
    ProcessOutput run_host_program_only(ProgramRequest request, const Cancel& cancel = {}) const;
    ProcessOutput run_shell(const ShellRequest& request, const Cancel& cancel = {}) const;
    ProcessOutput run_host_shell_only(const ShellRequest& request, const Cancel& cancel = {}) const;
    ProcessOutput run_inspection_shell(const ShellRequest& request, const Cancel& cancel = {}) const;
    std::vector<std::string> get_versions(bool force = false, const Cancel& cancel = {});
    std::optional<std::vector<std::string>> cached_versions() const;
};
// Internal executable mode for the explicitly enabled Windows UAC path.
int elevated_shell_worker(const fs::path& request_path);
} // namespace devbox
