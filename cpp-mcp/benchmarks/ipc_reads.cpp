// Same-binary private coordinator timing. No production configuration or paid request.
#include "devbox/state_coordinator.hpp"
#include <algorithm>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
Json measure(std::string_view name, unsigned samples, const std::function<std::uint64_t()>& fn) {
    for (unsigned i = 0; i < 16; ++i)
        (void)fn();
    std::vector<double> us;
    us.reserve(samples);
    std::uint64_t checksum = 0;
    for (unsigned i = 0; i < samples; ++i) {
        const auto start = Clock::now();
        const auto value = fn();
        const auto duration = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        checksum += value;
        us.push_back(duration);
    }
    std::sort(us.begin(), us.end());
    return Json{{"operation", name},
                {"samples", samples},
                {"unit", "microseconds"},
                {"p50", us[(us.size() - 1) / 2]},
                {"p95", us[(us.size() - 1) * 95 / 100]},
                {"p99", us[(us.size() - 1) * 99 / 100]},
                {"checksum", checksum}};
}
int run(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
        return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
    require(argc == 1 || argc == 3, "Usage: devbox-ipc-read-bench [NEW_FIXTURE SAMPLES]");
    const bool smoke = argc == 1;
    const unsigned samples = smoke ? 12 : static_cast<unsigned>(std::stoul(argv[2]));
    // Preserve the coordinator's replay window/capacity. Never stress by disabling it.
    require(samples >= 12 && samples <= 512, "sample bound 12..512");
    const auto root = smoke ? fs::canonical(fs::temp_directory_path()) / ("devbox-ipc-bench-" + uuid())
                            : fs::absolute(path_from_utf8(argv[1]));
    require(!fs::exists(root), "new private fixture required");
    ensure_private_state_directory(root);
    const auto directory = root / "state";
    ScopeExit cleanup([&] {
        if (stop_state_coordinator(directory) && smoke) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    });
    StateClientOptions old;
    old.reuse_read_connections = false;
    StateClientOptions next;
    next.reuse_read_connections = true;
    auto baseline = open_coordinated_state(directory, old),
         candidate = open_coordinated_state(directory, next);
    StateMutation seed{{"task", "fixture", "owner", "scope", "ready", 0,
                        Json{{"payload", std::string(128, 'x')}, {"iteration", 1}}},
                       0};
    baseline->apply({&seed, 1});
    Json report{{"schema", 1},
                {"same_binary", true},
                {"mode_order", "ABBAABBA"},
                {"production_requests", 0},
                {"scope", "encrypted authenticated loopback coordinator RPC including process-instance "
                          "checks and result oracles"},
                {"writes_use_fresh_transport", true},
                {"trials", Json::array()}};
    for (unsigned trial = 0; trial < 8; ++trial) {
        const bool reuse = (trial % 4 == 1 || trial % 4 == 2);
        auto& store = reuse ? candidate : baseline;
        Json operations = Json::array();
        operations.push_back(measure("coordinator_get", samples, [&] {
            const auto value = store->get("task", "fixture");
            require(value && value->revision == 1 && value->principal == "owner" &&
                        value->data == seed.record.data,
                    "read oracle");
            return value->revision;
        }));
        operations.push_back(measure("coordinator_count", samples, [&] {
            const auto n = store->count("task", "owner");
            require(n == 1, "count oracle");
            return n;
        }));
        operations.push_back(measure("coordinator_miss", samples, [&] {
            require(!store->get("task", "missing"), "miss oracle");
            return 1;
        }));
        report["trials"].push_back(Json{{"round", trial}, {"reuse", reuse}, {"operations", operations}});
    }
    std::cout << report.dump(2) << '\n';
    return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> owned;
    for (int i = 0; i < argc; ++i)
        owned.push_back(narrow(argv[i]));
    std::vector<char*> args;
    for (auto& value : owned)
        args.push_back(value.data());
    try {
        return run(argc, args.data());
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
#endif
