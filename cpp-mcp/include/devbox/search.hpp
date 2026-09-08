#pragma once
#include "runtime.hpp"
namespace devbox {
struct SearchRequest {
    std::string pattern, path, glob = "*";
    bool case_sensitive = false;
    std::size_t max_matches = 200, max_depth = 12;
    std::uint64_t max_file_bytes = 2097152;
    Millis timeout{30000};
    std::vector<std::string> exclude_directories;
    bool include_ignored = false;
};
class SearchService {
    std::shared_ptr<const Config> config_;

  public:
    explicit SearchService(std::shared_ptr<const Config> config) : config_(std::move(config)) {}
    ProcessOutput search(SearchRequest request, const Cancel& cancel = {}) const;
};
struct InspectFileRequest {
    std::string path;
    fs::path working_dir;
    std::optional<fs::path> resolved_path;
    std::size_t max_bytes = 65536;
};
fs::path resolve_host_path(std::string_view requested, const fs::path& working_dir);
// The legacy windows_host_* MCP tools keep Windows lexical path rules even
// when served on another OS; ordinary Devbox file tools use native paths.
fs::path resolve_windows_host_path(std::string_view requested, const fs::path& working_dir);
Json inspect_host_file(const Config& config, const RuntimeExecutor& runtime,
                       const InspectFileRequest& request, const Cancel& cancel = {});
} // namespace devbox
