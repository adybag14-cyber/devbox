#pragma once
#include "runtime.hpp"
#include <array>
#include <shared_mutex>
namespace devbox {
std::string normalize_posix_path(std::string_view workspace, std::string_view path);
class DockerFiles {
    std::shared_ptr<const Config> config_;
    std::array<std::shared_timed_mutex, 256> locks_;
    std::shared_timed_mutex& lock_for(std::string_view path);
    ProcessOutput python(std::string_view script, std::vector<std::string> args,
                         std::optional<std::string> input, Millis timeout, const Cancel& cancel,
                         std::optional<std::size_t> capture = {}) const;

  public:
    explicit DockerFiles(std::shared_ptr<const Config> config) : config_(std::move(config)) {}
    ProcessOutput list(std::string path, bool recursive, std::size_t max_depth, std::size_t max_entries,
                       Millis timeout, const std::vector<std::string>& excluded, const Cancel& cancel = {});
    ProcessOutput read_text(const std::string& path, std::size_t max_bytes, const Cancel& cancel = {});
    ProcessOutput write_text(const std::string& path, std::string content, bool append, bool create_dirs,
                             const Cancel& cancel = {});
    Json read_large(const std::string& path, std::uint64_t offset, std::size_t max_bytes,
                    const Cancel& cancel = {});
    Json write_large(const std::string& path, std::string content_base64, bool append, bool create_dirs,
                     const std::optional<std::string>& expected_sha256 = {}, const Cancel& cancel = {});
};
} // namespace devbox
