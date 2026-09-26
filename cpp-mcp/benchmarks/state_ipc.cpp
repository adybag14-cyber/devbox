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
    StateMutation write_seed{{"task", "write-fixed", "owner", "scope", "checkpoint", 0, Json{{"value", 0}}},
                             0};
    setup->apply({&write_seed, 1});
    unsigned committed = 0;
    Json write_trials = Json::array();
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
        const auto write_samples = std::min<unsigned long>(samples, 256);
        StateMutation mutation = write_seed;
        StateEvent event{"write-events", "committed", 0, Json::object()};
        std::string batch;
        for (const bool replay : {false, true}) {
            std::vector<double> values;
            values.reserve(write_samples);
            for (unsigned i = 0; i < write_samples; ++i) {
                if (!replay) {
                    mutation.expected_revision = ++committed;
                    mutation.record.data["value"] = committed;
                    event.data["value"] = committed;
                    batch = "batch-" + std::to_string(committed);
                }
                const auto begin = Clock::now();
                if (client->apply_once(batch, {&mutation, 1}, {&event, 1}) != replay)
                    throw Error("Incorrect durable write/replay acknowledgement");
                values.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            auto ranked = values;
            std::sort(ranked.begin(), ranked.end());
            write_trials.push_back(Json{{"reuse", reuse},
                                        {"operation", replay ? "receipt_replay" : "durable_write"},
                                        {"samples", write_samples},
                                        {"p50_us", ranked[write_samples / 2]},
                                        {"p95_us", ranked[write_samples * 95 / 100]},
                                        {"p99_us", ranked[write_samples * 99 / 100]},
                                        {"samples_us", values}});
        }
        const auto state = client->get("task", "write-fixed");
        if (!state || state->revision != committed + 1 || state->data["value"] != committed)
            throw Error("Durable write receipt repeated or lost a transaction");
        trials.push_back(Json{{"reuse", reuse},
                              {"samples", samples},
                              {"wall_ms", wall},
                              {"p50_us", sorted[samples / 2]},
                              {"p95_us", sorted[samples * 95 / 100]},
                              {"p99_us", sorted[samples * 99 / 100]},
                              {"samples_us", times}});
    }
    std::uint64_t after = 0, events = 0;
    for (;;) {
        const auto page = setup->events("write-events", after, 100);
        if (page.empty())
            break;
        for (const auto& event : page) {
            if (event.sequence != ++events || event.data["value"] != events)
                throw Error("Benchmark found duplicated or missing event");
        }
        after = page.back().sequence;
    }
    if (events != committed)
        throw Error("Commit/event count mismatch");
    std::cout << Json{{"ok", true},
                      {"order", "ABBAABBA"},
                      {"baseline", "ephemeral fixed-endpoint HTTP"},
                      {"candidate", "bounded verified connection reuse"},
                      {"trials", trials},
                      {"write_trials", write_trials},
                      {"durable_commits", committed},
                      {"events", events},
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
