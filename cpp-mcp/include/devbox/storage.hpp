#pragma once
#include "common.hpp"
#include "native.hpp"
#include "process.hpp"

namespace devbox {
struct FileState {
    bool exists = false;
    std::uint64_t bytes = 0;
    std::optional<std::string> sha256;
    Json json() const;
};
struct Preconditions {
    std::optional<std::string> sha256;
    std::optional<std::uint64_t> offset;
};
struct WriteReceipt {
    std::string path;
    FileState previous, current;
    bool replayed = false;
    Json json() const;
};
class FileLock {
    NativeHandle handle_;

  public:
    FileLock(const fs::path& path, Millis timeout = Millis(5000), const Cancel& cancel = {},
             bool private_file = false);
    FileLock(FileLock&&) noexcept = default;
    FileLock& operator=(FileLock&&) noexcept = default;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
};
FileState file_state(const fs::path& path);
WriteReceipt atomic_write(const fs::path& path, std::string_view payload, bool append = false,
                          bool create_dirs = true, const Preconditions& expected = {});
struct ArtifactChunk {
    fs::path path;
    std::uint64_t bytes = 0;
    std::string sha256;
};
// Reads each immutable chunk once into a bounded buffer, verifies chunk and whole hashes,
// then performs one atomic replacement guarded by the same lock as ordinary atomic writes.
WriteReceipt publish_chunks(const fs::path& destination, const fs::path& private_staging_file,
                            std::span<const ArtifactChunk> chunks, std::uint64_t bytes,
                            std::string_view whole_sha256, std::string_view expected_sha256,
                            const Cancel& cancel = {},
                            const std::function<void(std::string_view)>& transition_hook = {});
std::size_t atomic_lock_stripe(const fs::path& resolved);
fs::path canonical_target(const fs::path& path);
std::string read_text(const fs::path& path, std::size_t max_bytes);
Json read_large(const fs::path& path, std::uint64_t offset, std::size_t max_bytes);
Json write_large(const fs::path& path, std::string_view content_base64, bool append, bool create_dirs,
                 const std::optional<std::string>& expected_sha256 = {});
struct ListOptions {
    fs::path path;
    bool recursive = false;
    std::size_t max_depth = 4, max_entries = 1000;
    Millis timeout{30000};
    std::vector<std::string> exclude_directories;
    // Independent bounds on enumeration work and buffered path/text payloads, including vector slack.
    std::size_t max_enumerated_entries = 16384, memory_budget_bytes = 4 * 1024 * 1024;
};
ProcessOutput list_files(const ListOptions& options, const Cancel& cancel = {});
void validate_key(std::string_view value);
Json task_get(const fs::path& root, std::string_view id);
Json task_put(const fs::path& root, std::string_view id, std::uint64_t revision, const Json& state);
Json task_list(const fs::path& root, const std::optional<std::string>& cursor = {}, std::size_t limit = 50);
} // namespace devbox
