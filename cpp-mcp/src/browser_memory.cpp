#include "devbox/browser_memory.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>

namespace devbox {
namespace {
constexpr std::size_t membership_limit = 16384, mountinfo_limit = 1024 * 1024, group_limit = 256;
std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n'))
        value.remove_suffix(1);
    return value;
}
std::optional<std::uint64_t> number(std::string_view text) {
    text = trim(text);
    if (text.empty())
        return {};
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return {};
    return value;
}
bool token(std::string_view text, std::string_view sought) {
    for (;;) {
        const auto end = text.find(',');
        if (text.substr(0, end) == sought)
            return true;
        if (end == text.npos)
            return false;
        text.remove_prefix(end + 1);
    }
}
bool path_valid(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > 4096 ||
        path.find_first_of(std::string_view("\0\r\n", 3)) != path.npos)
        return false;
    if (path == "/")
        return true;
    path.remove_prefix(1);
    for (;;) {
        const auto end = path.find('/');
        const auto component = path.substr(0, end);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == path.npos)
            return true;
        path.remove_prefix(end + 1);
    }
}
std::optional<std::string> mount_path(std::string_view value) {
    if (value.size() > 16384)
        return {};
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\') {
            result.push_back(value[index]);
            continue;
        }
        if (value.size() - index < 4)
            return {};
        unsigned decoded = 0;
        for (unsigned offset = 1; offset <= 3; ++offset) {
            const auto digit = value[index + offset];
            if (digit < '0' || digit > '7')
                return {};
            decoded = decoded * 8 + static_cast<unsigned>(digit - '0');
        }
        if (decoded > 255)
            return {};
        result.push_back(static_cast<char>(decoded));
        index += 3;
    }
    return path_valid(result) ? std::optional(std::move(result)) : std::nullopt;
}
[[maybe_unused]] std::optional<std::string> bounded_read(std::string_view path, std::size_t limit) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file)
        return {};
    std::string result;
    std::array<char, 4096> buffer{};
    while (file) {
        const auto wanted = std::min(buffer.size(), limit - result.size() + 1);
        file.read(buffer.data(), static_cast<std::streamsize>(wanted));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > limit - result.size())
            return {};
        result.append(buffer.data(), count);
    }
    return file.eof() ? std::optional(std::move(result)) : std::nullopt;
}
} // namespace

std::vector<BrowserMemoryGroup> browser_memory_groups(std::string_view membership,
                                                      std::string_view mountinfo) {
    if (membership.size() > membership_limit || mountinfo.size() > mountinfo_limit)
        return {};
    std::array<std::string, 2> member;
    std::istringstream members{std::string(membership)};
    std::string line;
    while (std::getline(members, line)) {
        const auto first = line.find(':'),
                   second = first == line.npos ? line.npos : line.find(':', first + 1);
        if (second == line.npos)
            continue;
        const auto controllers = std::string_view(line).substr(first + 1, second - first - 1);
        const bool unified = first == 1 && line[0] == '0' && controllers.empty();
        if (!unified && !token(controllers, "memory"))
            continue;
        const auto path = line.substr(second + 1);
        if (!path_valid(path) || !member[unified ? 1 : 0].empty())
            return {};
        member[unified ? 1 : 0] = path;
    }
    std::vector<BrowserMemoryGroup> result;
    std::istringstream mounts{std::string(mountinfo)};
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::vector<std::string> words;
        std::string word;
        while (fields >> word) {
            if (words.size() >= 128)
                return {};
            words.push_back(std::move(word));
        }
        const auto separator = std::find(words.begin(), words.end(), "-");
        if (separator - words.begin() < 6 || words.end() - separator != 4)
            continue;
        const bool unified = separator[1] == "cgroup2";
        if (!unified && (separator[1] != "cgroup" || !token(separator[3], "memory")))
            continue;
        const auto& path = member[unified ? 1 : 0];
        const auto root = mount_path(words[3]), point = mount_path(words[4]);
        if (path.empty() || !root || !point)
            continue;
        std::string suffix;
        if (*root == "/")
            suffix = path == "/" ? "" : path;
        else if (path == *root)
            suffix = "";
        else if (path.starts_with(*root + "/"))
            suffix = path.substr(root->size());
        else
            continue; // This mount exposes another subtree, not our membership.
        std::string directory = *point == "/" ? (suffix.empty() ? "/" : suffix) : *point + suffix;
        if (!path_valid(directory))
            continue;
        bool leaf = true;
        for (;;) {
            const auto duplicate = std::find_if(result.begin(), result.end(), [&](const auto& group) {
                return group.unified == unified && group.directory == directory;
            });
            if (duplicate == result.end()) {
                if (result.size() >= group_limit)
                    return {};
                result.push_back({directory, unified, leaf});
            } else {
                duplicate->member = duplicate->member || leaf;
            }
            if (directory == *point)
                break;
            const auto slash = directory.rfind('/');
            directory.resize(slash ? slash : 1);
            leaf = false;
        }
    }
    return result;
}

std::uint64_t browser_cgroup_headroom(std::uint64_t limit, std::uint64_t usage,
                                      std::uint64_t inactive_file_cache) {
    // Non-atomic/inconsistent counters receive no cache credit. Never credit swap.
    const auto working = inactive_file_cache < usage ? usage - inactive_file_cache : usage;
    return limit > working ? limit - working : 0;
}
std::optional<std::uint64_t> browser_memory_headroom(std::string_view meminfo, std::string_view membership,
                                                     std::string_view mountinfo,
                                                     const BrowserMemoryReader& read) {
    std::optional<std::uint64_t> available;
    if (meminfo.size() <= 65536) {
        std::istringstream lines{std::string(meminfo)};
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string name, value, unit, extra;
            if (fields >> name >> value >> unit && name == "MemAvailable:" && unit == "kB" &&
                !(fields >> extra)) {
                if (const auto count = number(value); count && *count <= UINT64_MAX / 1024)
                    available = *count * 1024;
                break;
            }
        }
    }
    auto groups = browser_memory_groups(membership, mountinfo);
    if (groups.empty()) {
        // Preserve the namespace-root fallback when proc discovery is unavailable.
        groups = {{"/sys/fs/cgroup", true, true}, {"/sys/fs/cgroup/memory", false, true}};
    }
    for (const auto& group : groups) {
        const auto file = [&](std::string_view name, std::size_t limit) {
            return read((group.directory == "/" ? "" : group.directory) + "/" + std::string(name), limit);
        };
        bool hierarchical = true;
        if (!group.unified) {
            const auto hierarchy = file("memory.use_hierarchy", 64);
            hierarchical = !hierarchy || hierarchy->size() > 64 || number(*hierarchy) != 0;
        }
        if (!group.member && !hierarchical)
            continue;
        const auto cap = file(group.unified ? "memory.max" : "memory.limit_in_bytes", 64);
        const auto limit = cap && cap->size() <= 64 ? number(*cap) : std::nullopt;
        if (!limit || *limit >= (1ULL << 60))
            continue;
        const auto used = file(group.unified ? "memory.current" : "memory.usage_in_bytes", 64);
        const auto current = used && used->size() <= 64 ? number(*used) : std::nullopt;
        if (!current)
            continue;
        std::uint64_t inactive = 0;
        const auto stats = file("memory.stat", 65536);
        if (stats && stats->size() <= 65536) {
            std::istringstream fields(*stats);
            std::string key, value;
            bool found = false;
            while (fields >> key >> value) {
                if (key != (group.unified || !hierarchical ? "inactive_file" : "total_inactive_file"))
                    continue;
                if (found) {
                    inactive = 0;
                    break;
                }
                inactive = number(value).value_or(0);
                found = true;
            }
        }
        const auto left = browser_cgroup_headroom(*limit, *current, inactive);
        available = available ? std::min(*available, left) : left;
    }
    return available;
}
std::optional<std::uint64_t> browser_available_memory() {
#if defined(__linux__)
    const auto meminfo = bounded_read("/proc/meminfo", 65536);
    const auto membership = bounded_read("/proc/self/cgroup", membership_limit);
    const auto mounts = bounded_read("/proc/self/mountinfo", mountinfo_limit);
    return browser_memory_headroom(meminfo.value_or(""), membership.value_or(""), mounts.value_or(""),
                                   bounded_read);
#else
    return {};
#endif
}
} // namespace devbox
