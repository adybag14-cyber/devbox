#include "devbox/contract.hpp"
#include "devbox/state_coordinator.hpp"
#include <algorithm>
#include <iostream>
using namespace devbox;
namespace {
int run(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
        return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
    const auto samples = argc == 3 && std::string_view(argv[1]) == "--samples" ? std::stoul(argv[2]) : 64UL;
    if ((argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--samples")) || samples < 32 ||
        samples > 1000)
        throw Error("Usage: devbox-state-ipc-bench [--samples 32..1000]");
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-state-ipc-bench-" + uuid());
    ensure_private_state_directory(root);
    ScopeExit cleanup([&] {
        if (stop_state_coordinator(root / "state")) {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    });
    auto setup = open_coordinated_state(root / "state");
    StateMutation seed{
        {"task", "fixed", "owner", "scope", "checkpoint", 0,
         Json{{"payload", std::string(128, 'x')}, {"literal", "${NO_EXPANSION}\\n"}, {"expected", 4265}}},
        0};
    setup->apply({&seed, 1});
    Json trials = Json::array();
    for (bool reuse : {false, true, true, false, false, true, true, false}) {
        StateClientOptions options;
        options.reuse_connections = reuse;
        auto client = open_coordinated_state(root / "state", options);
        const auto read = [&] {
            auto record = client->get("task", "fixed");
            if (!record || record->revision != 1 || record->principal != "owner" ||
                record->data != seed.record.data)
                throw Error("State IPC benchmark result mismatch");
        };
        for (unsigned i = 0; i < 32; ++i)
            read();
        std::vector<double> times;
        times.reserve(samples);
        const auto start = Clock::now();
        for (unsigned i = 0; i < samples; ++i) {
            const auto begin = Clock::now();
            read();
            times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
        }
        const auto wall = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        auto sorted = times;
        std::sort(sorted.begin(), sorted.end());
        trials.push_back(Json{{"reuse", reuse},
                              {"samples", samples},
                              {"wall_ms", wall},
                              {"p50_us", sorted[samples / 2]},
                              {"p95_us", sorted[samples * 95 / 100]},
                              {"p99_us", sorted[samples * 99 / 100]},
                              {"samples_us", times}});
    }
    std::cout << Json{{"ok", true},
                      {"order", "ABBAABBA"},
                      {"baseline", "ephemeral fixed-endpoint HTTP"},
                      {"candidate", "bounded verified connection reuse"},
                      {"trials", trials},
                      {"build", build_snapshot()},
                      {"production_requests", 0},
                      {"scope", "native coordinated read including encryption, proof checks, HTTP and "
                                "SQLite; no external MCP frontend"}}
                     .dump()
              << '\n';
    return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** wide) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i)
            args.push_back(narrow(wide[i]));
        std::vector<char*> values;
        for (auto& arg : args)
            values.push_back(arg.data());
        return run(argc, values.data());
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
