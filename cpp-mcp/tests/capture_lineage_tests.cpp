#include "devbox/capture.hpp"
#include <iostream>
#include <set>
using namespace devbox;
int main() {
    try {
        // PID 100 has been reused. The old service and its descendants still
        // contain that numeric PPID, but predate the new capture target.
        const std::map<std::uint32_t, std::uint32_t> parents{{100, 1},   {200, 100}, {201, 200}, {300, 100},
                                                             {301, 300}, {400, 100}, {500, 1}};
        const std::map<std::uint32_t, std::uint64_t> births{{100, 1000}, {200, 500},  {201, 600},
                                                            {300, 1100}, {301, 1200}, {500, 900}};
        std::set<std::uint32_t> queried;
        const auto result =
            capture_process_lineage(parents, 100, [&](std::uint32_t pid) -> std::optional<std::uint64_t> {
                queried.insert(pid);
                const auto row = births.find(pid);
                return row == births.end() ? std::nullopt : std::optional(row->second);
            });
        if (result != std::map<std::uint32_t, std::uint64_t>{{100, 1000}, {300, 1100}, {301, 1200}})
            throw Error("Reused PPID or unverifiable process entered capture candidates");
        if (queried.contains(201) || queried.contains(500))
            throw Error("Discovery should not query excluded branches or unrelated processes");
        bool rejected = false;
        try {
            (void)capture_process_lineage(parents, 100,
                                          [](std::uint32_t) -> std::optional<std::uint64_t> { return {}; });
        } catch (const Error&) {
            rejected = true;
        }
        if (!rejected)
            throw Error("Unverifiable capture root was accepted");
        std::cout << "capture lineage excludes recycled parents and preserves live descendants\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
