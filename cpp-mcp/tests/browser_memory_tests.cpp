#include "devbox/browser_memory.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Files {
    std::map<std::string, std::string, std::less<>> values;
    std::vector<std::string> reads;
    BrowserMemoryReader reader() {
        return [this](std::string_view path, std::size_t limit) -> std::optional<std::string> {
            reads.emplace_back(path);
            const auto found = values.find(path);
            if (found == values.end() || found->second.size() > limit)
                return {};
            return found->second;
        };
    }
};
constexpr std::string_view v1_mount =
    "30 20 0:25 / /sys/fs/cgroup/memory rw,nosuid,nodev,noexec - cgroup cgroup rw,memory\n";
constexpr std::string_view v2_mount = "31 20 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n";
} // namespace
int main() {
    try {
        // A systemd service under an unlimited mount root must observe its own
        // tighter budget. This is the topology missed by the old root-only read.
        Files v1{
            {
                {"/sys/fs/cgroup/memory/memory.limit_in_bytes", "9223372036854771712\n"},
                {"/sys/fs/cgroup/memory/system.slice/devbox.service/memory.limit_in_bytes", "629145600\n"},
                {"/sys/fs/cgroup/memory/system.slice/devbox.service/memory.usage_in_bytes", "576716800\n"},
                {"/sys/fs/cgroup/memory/system.slice/devbox.service/memory.stat",
                 "total_inactive_file 20971520\n"},
            },
            {}};
        const auto legacy = browser_memory_headroom(
            "MemAvailable: 819200 kB\n", "6:memory:/system.slice/devbox.service\n", v1_mount, v1.reader());
        require(legacy == 73400320, "Systemd leaf budget hidden by unlimited cgroup mount root");
        require(legacy < 819200ULL * 1024, "Host memory must not override a tighter service budget");

        Files v2{{
                     {"/sys/fs/cgroup/memory.max", "max\n"},
                     {"/sys/fs/cgroup/parent/memory.max", "500\n"},
                     {"/sys/fs/cgroup/parent/memory.current", "450\n"},
                     {"/sys/fs/cgroup/parent/memory.stat", "inactive_file 50\n"},
                     {"/sys/fs/cgroup/parent/leaf/memory.max", "1000\n"},
                     {"/sys/fs/cgroup/parent/leaf/memory.current", "100\n"},
                     {"/sys/fs/cgroup/parent/leaf/memory.stat", "inactive_file 20\n"},
                     {"/unrelated/memory.max", "1\n"},
                     {"/unrelated/memory.current", "1\n"},
                 },
                 {}};
        const auto mounts =
            std::string(v2_mount) + "32 20 0:26 /elsewhere /unrelated rw - cgroup2 cgroup rw\n";
        require(browser_memory_headroom("MemAvailable: 1 kB\n", "0::/parent/leaf\n", mounts, v2.reader()) ==
                    100,
                "Tighter ancestor headroom must account for sibling charges");
        require(std::none_of(v2.reads.begin(), v2.reads.end(),
                             [](const auto& path) { return path.starts_with("/unrelated"); }),
                "Unrelated cgroup subtree was sampled");
        require(browser_memory_headroom("MemAvailable: 0 kB\n", "0::/parent/leaf\n", mounts, v2.reader()) ==
                    0,
                "Exhausted host memory must remain the tighter bound");

        const auto bind = browser_memory_groups("0::/org/team/job\n",
                                                "41 20 0:26 /org /visible rw shared:1 - cgroup2 cgroup rw\n");
        require(bind.size() == 3 && bind[0].directory == "/visible/team/job" && bind[0].member &&
                    bind[1].directory == "/visible/team" && !bind[1].member &&
                    bind[2].directory == "/visible",
                "Bind-mount rebasing or ancestor boundary is wrong");
        const auto namespaced =
            browser_memory_groups("0::/\n", "41 20 0:26 / /budget\\040space rw - cgroup2 cgroup rw\n");
        require(namespaced.size() == 1 && namespaced[0].directory == "/budget space" && namespaced[0].member,
                "Namespace root or escaped mount point was not decoded");
        require(browser_memory_groups("6:memoryx:/unit\n", v1_mount).empty(),
                "Controller substring accepted");
        require(browser_memory_groups("6:cpu,memory:/unit\n", v1_mount).size() == 2,
                "Combined controller lost");
        require(browser_memory_groups("0::/a/../escape\n", v2_mount).empty(), "Membership escaped its mount");
        require(browser_memory_groups("0::/unit\n", "31 20 0:26 / /bad\\000path rw - cgroup2 cgroup rw\n")
                    .empty(),
                "NUL mount path accepted");
        require(browser_memory_groups("0::/unit\n", "31 20 0:26 / /bad\\999path rw - cgroup2 cgroup rw\n")
                    .empty(),
                "Malformed mount escape accepted");
        require(browser_memory_groups("0::/unit\n0::/other\n", v2_mount).empty(),
                "Ambiguous membership accepted");
        require(browser_memory_groups(std::string(16385, 'a'), v2_mount).empty(),
                "Membership read is unbounded");
        require(browser_memory_groups("0::/\n", std::string(1024 * 1024 + 1, 'a')).empty(),
                "Mount read is unbounded");
        std::string deep = "0::";
        for (unsigned i = 0; i < 257; ++i)
            deep += "/a";
        require(browser_memory_groups(deep, v2_mount).empty(), "Ancestor traversal is unbounded");

        Files hierarchy{{
                            {"/sys/fs/cgroup/memory/unit/memory.limit_in_bytes", "2048"},
                            {"/sys/fs/cgroup/memory/unit/memory.usage_in_bytes", "1024"},
                            {"/sys/fs/cgroup/memory/unit/memory.stat", "total_inactive_file 256"},
                            {"/sys/fs/cgroup/memory/memory.use_hierarchy", "0"},
                            {"/sys/fs/cgroup/memory/memory.limit_in_bytes", "1"},
                            {"/sys/fs/cgroup/memory/memory.usage_in_bytes", "1"},
                        },
                        {}};
        require(browser_memory_headroom("", "6:memory:/unit\n", v1_mount, hierarchy.reader()) == 1280,
                "Non-hierarchical legacy parent was applied to its child");
        hierarchy.values["/sys/fs/cgroup/memory/unit/memory.use_hierarchy"] = "0";
        hierarchy.values["/sys/fs/cgroup/memory/unit/memory.stat"] =
            "inactive_file 32\ntotal_inactive_file 256\n";
        require(browser_memory_headroom("", "6:memory:/unit\n", v1_mount, hierarchy.reader()) == 1056,
                "Non-hierarchical member credited its descendants' cache");

        Files counters{{{"/sys/fs/cgroup/memory.max", "512"},
                        {"/sys/fs/cgroup/memory.current", "400"},
                        {"/sys/fs/cgroup/memory.stat", "inactive_file 500\n"}},
                       {}};
        const auto sample = [&] {
            return browser_memory_headroom("", "0::/\n", v2_mount, counters.reader());
        };
        require(sample() == 112, "Inconsistent cache counters received credit");
        counters.values["/sys/fs/cgroup/memory.stat"] = "inactive_file 100\ninactive_file 200\n";
        require(sample() == 112, "Duplicate cache fields received credit");
        counters.values["/sys/fs/cgroup/memory.current"] = "700";
        counters.values["/sys/fs/cgroup/memory.stat"] = "inactive_file 50";
        require(sample() == 0, "Over-limit working set wrapped around");
        counters.values["/sys/fs/cgroup/memory.current"] = "400 trailing";
        require(!sample(), "Malformed usage counter accepted");
        counters.values["/sys/fs/cgroup/memory.current"] = "400";
        for (const auto invalid : {"max", "-1", "18446744073709551616", "512x", ""}) {
            counters.values["/sys/fs/cgroup/memory.max"] = invalid;
            require(!sample(), "Invalid or unbounded cap accepted");
        }
        counters.values["/sys/fs/cgroup/memory.max"] = "0";
        require(sample() == 0, "Zero limit ignored");
        counters.values["/sys/fs/cgroup/memory.max"] = "512";
        require(browser_memory_headroom("", "", "", counters.reader()) == 162,
                "Existing namespace-root fallback was lost");
        Files empty;
        require(!browser_memory_headroom("MemAvailable: 18446744073709551615 kB\n", "", "", empty.reader()),
                "Host counter byte conversion overflowed");
        require(!browser_memory_headroom("MemAvailable: 123 MB\n", "", "", empty.reader()),
                "Unexpected host counter unit accepted");
        std::cout << "Native memory: nested v1/v2, ancestors, namespace/bind roots, strict counters and "
                     "bounded paths passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
