#pragma once
#include "common.hpp"
#include <deque>
#include <map>

namespace devbox {
using Environment = std::map<std::string, std::string>;
enum class OutputStream { stdout_stream, stderr_stream };
struct CaptureResult {
    std::string text;
    std::size_t original_chars = 0;
    bool truncated = false;
};
// Counts Unicode scalar values, matching the Rust process capture contract.
class CaptureAccumulator {
    std::optional<std::size_t> limit_;
    std::string pending_;
    std::deque<std::string> head_, tail_;
    std::size_t count_ = 0;
    bool truncated_ = false;
    void push_text(std::string_view text);

  public:
    explicit CaptureAccumulator(std::optional<std::size_t> limit) : limit_(limit) {}
    void push(std::string_view bytes);
    void finish();
    CaptureResult snapshot() const;
};
struct ProcessOptions {
    std::optional<fs::path> cwd;
    // When present, this is the complete child environment, not a global mutation.
    std::optional<Environment> env;
    std::optional<Millis> timeout;
    Millis termination_grace{3000};
    std::optional<std::size_t> max_capture_chars;
    std::optional<std::string> input;
    std::function<void(OutputStream, std::string_view)> on_output;
    std::function<void(std::uint32_t)> on_pid;
    // Only shell adapters may use this to supply the Windows shell's native syntax.
    std::optional<std::string> windows_raw_arguments;
};
struct ProcessOutput {
    std::string stdout_text, stderr_text;
    std::size_t stdout_original_chars = 0, stderr_original_chars = 0;
    bool stdout_capture_truncated = false, stderr_capture_truncated = false;
    int exit_code = 0;
    std::uint32_t pid = 0;
    std::uint64_t elapsed_ms = 0;
};
struct ProcessError : Error {
    std::optional<int> exit_code, signal;
    std::string stdout_text, stderr_text, file;
    std::vector<std::string> args;
    bool timed_out = false, aborted = false;
    std::uint64_t elapsed_ms = 0;
    using Error::Error;
};
Environment current_environment();
std::optional<fs::path> find_program(std::string_view program, const Environment* env = nullptr);
std::string quote_windows_argument(std::string_view value);
bool is_administrator();
bool process_alive(std::uint32_t pid);
std::optional<std::uint64_t> process_instance(std::uint32_t pid);
bool process_matches_instance(std::uint32_t pid, std::optional<std::uint64_t> expected);
// Only callers holding a stored process identity may request cross-process termination.
bool terminate_process_tree(std::uint32_t pid, std::optional<std::uint64_t> expected);
ProcessOutput spawn_process(std::string_view file, const std::vector<std::string>& args,
                            const ProcessOptions& options = {}, const Cancel& cancel = {});
std::string summarize_process_failure(std::string_view file, int code, std::string_view stdout_text,
                                      std::string_view stderr_text);
CaptureResult read_text_file_bounded(const fs::path& path, std::optional<std::size_t> limit);
} // namespace devbox
