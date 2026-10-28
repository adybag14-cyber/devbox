#include "devbox/allocator.hpp"
#include "devbox/engine.hpp"
#include "devbox/filesystem_worker.hpp"
#include <iostream>

using namespace devbox;
namespace {
template <class Fn> void phase(std::string_view name, Fn work) {
    const auto before = allocator_counters();
    const auto start = Clock::now();
    work();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const auto after = allocator_counters();
    std::cout << Json{{"phase", name},
                      {"seconds", elapsed},
                      {"allocation_calls", after.calls - before.calls},
                      {"allocated_bytes", after.allocated - before.allocated},
                      {"current_bytes", after.current},
                      {"retained_bytes_delta",
                       static_cast<std::int64_t>(after.current) - static_cast<std::int64_t>(before.current)}}
                     .dump()
              << '\n'
              << std::flush;
}
int run(const fs::path& root) {
    if (!root.is_absolute() || !fs::create_directory(root))
        throw Error("Supply a new absolute fixture directory");
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->host = "127.0.0.1";
    config->port = 0;
    config->platform = Platform::detect();
    config->runtime_mode = RuntimeMode::host;
    config->devbox_auto_start = false;
    config->host_default_workdir = root;
    config->host_workspace_path = root;
    config->devbox_workspace_path = root;
    config->jobs_root = root / "jobs";
    config->state_root = root / "run" / "state";
    config->state_backend = "sqlite";
    config->execution_slot_root = root / "slots";
    config->mcp_performance_state_path = root / "run" / "performance.json";
    config->node_exe = "node";
#ifdef _WIN32
    config->host_shell = "cmd.exe";
    config->power_shell_exe = "powershell.exe";
#else
    config->host_shell = "/bin/sh";
#endif
    auto engine = std::make_shared<Engine>(config);
    HttpServer server(config, engine);
    ScopeExit stop([&] {
        server.stop();
        engine->stop();
        (void)stop_state_coordinator(config->state_root);
    });
    // Readiness alone does not prove a working durable backend. Refuse a
    // degraded benchmark (for example a wrapper that forbids coordinator
    // breakaway) before measuring idle work or HTTP requests.
    StateClientOptions state_options;
    state_options.executable = executable_path();
    const auto state = open_coordinated_state(config->state_root, state_options);
    (void)state->diagnostics();
    const auto base = "http://127.0.0.1:" + std::to_string(server.start());
    engine->attach(server);
    const auto deadline = Clock::now() + Millis(10000);
    while (!engine->ready() && Clock::now() < deadline)
        std::this_thread::sleep_for(Millis(10));
    if (!engine->ready())
        throw Error("Isolated server not ready");
    const auto get = [&](const std::string& path) {
        const auto result =
            http_request("GET", base + path, {}, Json::object(), Millis(5000), 1024 * 1024, {}, true);
        if (result.status != 200)
            throw Error("Isolated HTTP check failed");
    };
    std::cout << Json{{"benchmark", "server-lifetime"},
                      {"build", build_snapshot()},
                      {"scope", "Isolated loopback engine and client; no production state"}}
                     .dump()
              << '\n';
    phase("idle_20_seconds", [] { std::this_thread::sleep_for(Millis(20000)); });
    phase("readyz_200_requests", [&] {
        for (unsigned n = 0; n < 200; ++n)
            get("/readyz");
    });
    phase("metadata_200_requests", [&] {
        for (unsigned n = 0; n < 200; ++n)
            get("/");
    });
    phase("idle_after_20_seconds", [] { std::this_thread::sleep_for(Millis(20000)); });
    for (const auto* name : {"job-maintenance.json", "job-quota.json"}) {
        const auto status = read_json(root / "run" / name);
        if (!status.value("error", Json()).is_null())
            throw Error("Isolated maintenance worker failed: " + status.at("error").dump());
    }
    std::cout << Json{{"resources", server.resource_snapshot()}, {"ready", engine->ready()}}.dump() << '\n';
    phase("stop", [&] {
        server.stop();
        engine->stop();
    });
    if (!stop_state_coordinator(config->state_root))
        throw Error("Owned coordinator did not stop");
    stop.disarm();
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
            return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
        if (argc == 3 && std::string_view(argv[1]) == "--stop-fixture-coordinator")
            return stop_state_coordinator(path_from_utf8(argv[2])) ? 0 : 1;
        if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
            return run_filesystem_worker();
        if (argc != 2)
            throw Error("Usage: devbox-server-lifetime-bench NEW_FIXTURE_DIRECTORY");
        return run(path_from_utf8(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
