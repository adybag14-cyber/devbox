#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devbox {
struct BrowserMemoryGroup {
    std::string directory;
    bool unified = false;
    bool member = false;
};
using BrowserMemoryReader =
    std::function<std::optional<std::string>(std::string_view path, std::size_t max_bytes)>;
// Pure bounded discovery for qualification; no paths or reader are MCP arguments.
std::vector<BrowserMemoryGroup> browser_memory_groups(std::string_view membership,
                                                      std::string_view mountinfo);
std::uint64_t browser_cgroup_headroom(std::uint64_t limit, std::uint64_t usage,
                                      std::uint64_t inactive_file_cache);
std::optional<std::uint64_t> browser_memory_headroom(std::string_view meminfo, std::string_view membership,
                                                     std::string_view mountinfo,
                                                     const BrowserMemoryReader& read);
std::optional<std::uint64_t> browser_available_memory();
} // namespace devbox
