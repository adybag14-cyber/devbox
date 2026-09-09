#include "devbox/contract.hpp"
#include "devbox/engine.hpp"
#include "devbox/result.hpp"
#include "devbox/scheduler.hpp"
#include "devbox/storage.hpp"
#include "devbox/telemetry.hpp"
#include <algorithm>
#include <iostream>

using namespace devbox;
namespace {
template <class Operation> void measure(const char* name, std::size_t count, Operation operation) {
    std::vector<double> samples;
    std::uint64_t observed = 0;
    for (std::size_t i = 0; i < count + 3; ++i) {
        const auto start = Clock::now();
        observed += static_cast<std::uint64_t>(operation(i));
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (i >= 3)
            samples.push_back(ms);
    }
    std::sort(samples.begin(), samples.end());
    std::cout << Json{{"component", name},
                      {"count", count},
                      {"p50Ms", samples[count / 2]},
                      {"p95Ms", samples[std::min(count - 1, count * 95 / 100)]},
                      {"observed", observed}}
                     .dump()
              << '\n'
              << std::flush;
}
int run(const fs::path& root) {
    if (!root.is_absolute() || !fs::create_directory(root))
        throw Error("Supply a new absolute fixture directory under an existing parent");
    std::cout << Json{{"build", build_snapshot()}, {"fixtureRoot", path_text(root)}}.dump() << '\n';
    const auto source = root / "source.bin", target = root / "atomic.bin";
    const std::string bytes(512 * 1024, 'x');
    write_file(source, bytes);
    auto data = read_large(source, 0, bytes.size());
    measure("sha256_512k", 80, [&](auto) { return sha256(bytes).size(); });
    measure("base64_512k", 80, [&](auto) { return base64_encode(bytes).size(); });
    measure("read_large_512k", 80, [&](auto) {
        return read_large(source, 0, bytes.size())["content_base64"].get_ref<const std::string&>().size();
    });
    measure("json_dump_512k", 80, [&](auto) { return data.dump().size(); });
    measure("json_output_512k", 80, [&](auto) { return json_dump(data).size(); });
    measure("result_512k", 80,
            [&](auto) { return result_explicit("Read bytes", data, "Read bytes").size(); });
    Config config;
    config.project_root = root;
    config.runtime_mode = RuntimeMode::host;
    config.jobs_root = root / "jobs";
    config.execution_slot_root = root / "scheduler";
    config.devbox_workspace_path = root;
    config.host_default_workdir = root;
    config.host_exec_enabled = true;
    config.host_program_allowlist = {"node"};
    config.platform = Platform::detect();
    config.mcp_performance_state_path = root / "run" / "performance.json";
    ToolContract contract(config);
    measure("validate_read_args", 80, [&](auto) {
        return contract
            .arguments("devbox_read_large_file",
                       Json{{"path", path_text(source)}, {"max_bytes", bytes.size()}})
            .size();
    });
    auto response = result_explicit("Read bytes", data, "Read bytes");
    {
        BackgroundTasks background;
        UsageTelemetry usage(config, background);
        measure("usage_start_finish_512k", 80, [&](auto) {
            const auto id =
                usage.started("devbox_read_large_file", Json{{"path", path_text(source)}}, Json::object());
            usage.finished(id, response);
            return id.size();
        });
        usage.stop();
        std::cout << Json{{"usage", usage.snapshot()}}.dump() << '\n';
    }
    write_file(target, std::string(4096, 'a'));
    auto previous = file_state(target).sha256;
    measure("atomic_write_4k", 20, [&](auto i) {
        auto receipt = atomic_write(target, std::string(4096, i % 2 ? 'a' : 'b'), false, true,
                                    Preconditions{previous, {}});
        if (receipt.replayed)
            throw Error("Timing probe unexpectedly replayed an atomic write");
        previous = receipt.current.sha256;
        return receipt.current.bytes;
    });
    ExecutionScheduler scheduler(SchedulerConfig::from(config));
    measure("scheduler_acquire_release", 20, [&](auto) {
        AcquireRequest request;
        request.kind = ExecutionKind::interactive;
        request.resource_class = ResourceClass::light;
        request.label = "component timing";
        const auto lease = scheduler.acquire(request);
        return lease.slots.size();
    });
    const auto node = find_program("node");
    if (!node)
        throw Error("Node is required for the process component timing probe");
    config.node_exe = path_text(*node);
    measure("native_node_direct", 12, [&](auto) {
        ProcessOptions options;
        options.cwd = root;
        options.timeout = Millis(10000);
        const auto out =
            spawn_process(path_text(*node), {"-e", "process.stdout.write('benchmark-ok')"}, options);
        if (out.stdout_text != "benchmark-ok")
            throw Error("Node timing probe returned incorrect output");
        return out.stdout_text.size();
    });
    measure("native_node_with_cancel", 12, [&](auto) {
        ProcessOptions options;
        options.cwd = root;
        options.timeout = Millis(10000);
        const auto out = spawn_process(path_text(*node), {"-e", "process.stdout.write('benchmark-ok')"},
                                       options, std::make_shared<Cancellation>());
        if (out.stdout_text != "benchmark-ok")
            throw Error("Cancellable Node timing probe returned incorrect output");
        return out.stdout_text.size();
    });
    auto engine = std::make_shared<Engine>(std::make_shared<Config>(config));
    asio::io_context io;
    const auto invoke = [&](const std::string& name, const Json& args) {
        const auto id = engine->tool_started(name, args, Json::object());
        io.restart();
        auto pending = asio::co_spawn(io, engine->call_tool(name, args, {}), asio::use_future);
        io.run();
        auto result = pending.get();
        engine->tool_finished(id, result);
        if (json_bool(result, "isError"))
            throw Error(result.dump());
        return result;
    };
    measure("engine_atomic_write_4k", 20, [&](auto i) {
        auto result = invoke("devbox_write_file_atomic",
                             Json{{"path", path_text(target)},
                                  {"content_base64", base64_encode(std::string(4096, i % 2 ? 'b' : 'a'))},
                                  {"expected_file_sha256", *previous}});
        const auto& receipt = result["structuredContent"]["data"];
        if (json_bool(receipt, "replayed"))
            throw Error("Engine component unexpectedly replayed an atomic write");
        previous = json_string(receipt["current"], "sha256");
        return result.size();
    });
    measure("engine_read_512k", 80, [&](auto) {
        return invoke("devbox_read_large_file",
                      Json{{"path", path_text(source)}, {"max_bytes", bytes.size()}})
            .size();
    });
    measure("engine_node", 12, [&](auto) {
        return invoke("host_run_program",
                      Json{{"program", "node"},
                           {"args", Json::array({"-e", "process.stdout.write('benchmark-ok')"})},
                           {"working_dir", path_text(root)},
                           {"timeout_seconds", 10}})
            .size();
    });
    engine->stop();
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw Error("Usage: devbox-component-bench NEW_FIXTURE_DIRECTORY");
        return run(path_from_utf8(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
