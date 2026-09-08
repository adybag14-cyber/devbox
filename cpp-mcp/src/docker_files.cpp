#include "devbox/docker_files.hpp"
#include "devbox/container_helpers.hpp"
namespace devbox {
namespace {
template <class Lock> void acquire(Lock& lock, const Cancel& cancel) {
    while (!lock.try_lock_for(Millis(20)))
        if (cancel)
            cancel->check();
    if (cancel)
        cancel->check();
}
Json parse_result(const ProcessOutput& output, const std::string& path, bool write) {
    try {
        return Json::parse(output.stdout_text);
    } catch (const std::exception& e) {
        ProcessError error("Large file " + std::string(write ? "write" : "read") + " for " + path +
                           " returned invalid JSON: " + e.what());
        error.stdout_text = output.stdout_text;
        error.stderr_text = output.stderr_text;
        error.exit_code = output.exit_code;
        throw error;
    }
}
} // namespace
std::string normalize_posix_path(std::string_view workspace, std::string_view path) {
    auto value = path.starts_with('/') ? std::string(path) : std::string(workspace) + "/" + std::string(path);
    std::vector<std::string> pieces;
    for (const auto& part : split(value, '/', false)) {
        if (part == ".")
            continue;
        if (part == "..") {
            if (!pieces.empty())
                pieces.pop_back();
        } else
            pieces.push_back(part);
    }
    return "/" + join(pieces, "/");
}
std::shared_timed_mutex& DockerFiles::lock_for(std::string_view path) {
    const auto key = normalize_posix_path(path_text(config_->devbox_workspace_path), path);
    const auto digest = sha256(key);
    return locks_[std::stoul(digest.substr(0, 2), nullptr, 16)];
}
ProcessOutput DockerFiles::python(std::string_view script, std::vector<std::string> args,
                                  std::optional<std::string> input, Millis timeout, const Cancel& cancel,
                                  std::optional<std::size_t> capture) const {
    std::vector<std::string> command{"exec"};
    if (input)
        command.emplace_back("-i");
    if (!config_->devbox_default_user.empty())
        command.insert(command.end(), {"-u", config_->devbox_default_user});
    command.insert(command.end(), {"-w", path_text(config_->devbox_workspace_path),
                                   config_->devbox_container_name, "python3", "-c", std::string(script)});
    command.insert(command.end(), args.begin(), args.end());
    ProcessOptions options;
    options.timeout = timeout;
    options.input = std::move(input);
    options.max_capture_chars =
        capture.value_or(std::min<std::size_t>(config_->max_mcp_transfer_chars, 8000000));
    return spawn_process("docker", command, options, cancel);
}
ProcessOutput DockerFiles::list(std::string path, bool recursive, std::size_t max_depth,
                                std::size_t max_entries, Millis timeout,
                                const std::vector<std::string>& excluded, const Cancel& cancel) {
    return python(container_helpers::list_python,
                  {std::move(path), recursive ? "1" : "0",
                   std::to_string(std::max<std::size_t>(1, max_depth)),
                   std::to_string(std::max<std::size_t>(1, max_entries)),
                   std::to_string(std::max<std::int64_t>(1, timeout.count())), Json(excluded).dump()},
                  {}, timeout, cancel);
}
ProcessOutput DockerFiles::read_text(const std::string& path, std::size_t max_bytes, const Cancel& cancel) {
    std::shared_lock lock(lock_for(path), std::defer_lock);
    acquire(lock, cancel);
    return python(container_helpers::read_text_python,
                  {path, std::to_string(std::max<std::size_t>(1, max_bytes))}, {}, Millis(30000), cancel);
}
ProcessOutput DockerFiles::write_text(const std::string& path, std::string content, bool append,
                                      bool create_dirs, const Cancel& cancel) {
    std::unique_lock lock(lock_for(path), std::defer_lock);
    acquire(lock, cancel);
    return python(container_helpers::write_text_python, {path, append ? "1" : "0", create_dirs ? "1" : "0"},
                  std::move(content), Millis(30000), cancel);
}
Json DockerFiles::read_large(const std::string& path, std::uint64_t offset, std::size_t max_bytes,
                             const Cancel& cancel) {
    std::shared_lock lock(lock_for(path), std::defer_lock);
    acquire(lock, cancel);
    const auto cap = ((std::max<std::size_t>(1, max_bytes) + 2) / 3) * 4 + 16384;
    return parse_result(
        python(container_helpers::large_read_python,
               {path, std::to_string(offset), std::to_string(std::max<std::size_t>(1, max_bytes))}, {},
               Millis(120000), cancel, cap),
        path, false);
}
Json DockerFiles::write_large(const std::string& path, std::string content_base64, bool append,
                              bool create_dirs, const std::optional<std::string>& expected_sha256,
                              const Cancel& cancel) {
    std::unique_lock lock(lock_for(path), std::defer_lock);
    acquire(lock, cancel);
    return parse_result(
        python(container_helpers::large_write_python,
               {path, append ? "1" : "0", create_dirs ? "1" : "0", expected_sha256.value_or("")},
               std::move(content_base64), Millis(120000), cancel),
        path, true);
}
} // namespace devbox
