#include "devbox/computer_use.hpp"
#include "devbox/engine.hpp"
#include "devbox/grants.hpp"
#include "devbox/state_coordinator.hpp"
#include "server_main.hpp"
#include <csignal>
#include <iostream>
namespace {
#ifdef _WIN32
volatile LONG shutdown_requested = 0;
void signal_handler(int) {
    InterlockedExchange(&shutdown_requested, 1);
    WakeByAddressAll(const_cast<LONG*>(&shutdown_requested));
}
#else
volatile std::sig_atomic_t shutdown_requested = 0;
void signal_handler(int) {
    shutdown_requested = 1;
}
#endif
#ifdef _WIN32
BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT) {
        signal_handler(0);
        return TRUE;
    }
    return FALSE;
}
#endif
} // namespace
int devbox::run_mcp(const std::vector<std::string>& args) {
    using namespace devbox;
#ifdef _WIN32
    // A headless service and its owned workers report launch/crash errors through the protocol.
    // Windows critical-error dialogs must not hold a failed worker open on the user's desktop.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    try {
        const auto mode = args.empty() ? "" : args.front();
        if (mode == "--linux-isolation-worker") {
            if (args.size() != 2)
                throw Error("Isolated worker requires a private request file");
            return run_linux_isolation_worker(path_from_utf8(args[1]));
        }
        if (mode == "--stop-state-coordinator") {
            if (args.size() != 2)
                throw Error("--stop-state-coordinator requires a private state directory");
            if (!stop_state_coordinator(path_from_utf8(args[1])))
                throw Error("STATE_COORDINATOR_STOP_UNCONFIRMED");
            return 0;
        }
        if (mode == "--state-coordinator") {
            if (args.size() != 2)
                throw Error("--state-coordinator requires a private state directory");
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
#ifdef _WIN32
            SetConsoleCtrlHandler(console_handler, TRUE);
            return run_state_coordinator(path_from_utf8(args[1]), [] {
                return InterlockedCompareExchange(&shutdown_requested, 0, 0) != 0;
            });
#else
            return run_state_coordinator(path_from_utf8(args[1]), [] { return shutdown_requested != 0; });
#endif
        }
        if (mode == "--capture-worker")
            return run_capture_worker(std::vector<std::string>(args.begin() + 1, args.end()));
        if (mode == "--computer-use-probe") {
            if (args.size() != 2)
                throw Error("--computer-use-probe requires one local pipe name");
            std::cout << computer_broker_call(args[1], "windows", Json::object(), {}).at("result").dump(2)
                      << '\n';
            return 0;
        }
        if (mode == "--computer-use-broker") {
            if (args.size() != 2)
                throw Error("--computer-use-broker requires one local pipe name");
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
#ifdef _WIN32
            SetConsoleCtrlHandler(console_handler, TRUE);
            return run_computer_broker(
                args[1], [] { return InterlockedCompareExchange(&shutdown_requested, 0, 0) != 0; });
#else
            return run_computer_broker(args[1], [] { return shutdown_requested != 0; });
#endif
        }
        if (mode == "--help" || mode == "-h") {
            std::cout << "Devbox C++ MCP " << build_version()
                      << "\nUsage: devbox-mcp [--build-info|--parity-report|--dump-contract|--job-runner "
                         "PATH|--elevated-shell-worker PATH|--capture-worker OUTPUT MODE QUALITY [PID TREE]|"
                         "--computer-use-broker PIPE|--computer-use-probe PIPE|--migrate-state|"
                         "--stop-state-coordinator ROOT|--grant-create-workspace ID|--grant-issue JSON|"
                         "--grant-revoke ID|--grant-inspect ID|--execute-granted JSON]\n";
            return 0;
        }
        if (mode == "--build-info") {
            std::cout << build_snapshot().dump(2) << '\n';
            return 0;
        }
        if (mode == "--elevated-shell-worker") {
            if (args.size() != 2)
                throw Error("--elevated-shell-worker requires one request path");
            return elevated_shell_worker(path_from_utf8(args[1]));
        }
        auto config = std::make_shared<Config>(Config::load(mode != "--job-runner"));
        if (mode == "--grant-create-workspace" || mode == "--grant-issue" || mode == "--grant-revoke" ||
            mode == "--grant-inspect" || mode == "--execute-granted") {
            if (args.size() != 2 || config->state_backend != "sqlite")
                throw Error("Grant administration requires one operand and MCP_STATE_BACKEND=sqlite");
            JobStore jobs(config);
            auto state = jobs.index();
            const auto private_root = config->state_root / "isolated";
            ensure_private_state_directory(private_root);
            if (mode == "--grant-create-workspace") {
                validate_key(args[1]);
                const auto workspace = private_root / args[1];
                ensure_private_state_directory(workspace);
                std::cout << Json{{"workspace", path_text(fs::canonical(workspace))}}.dump() << '\n';
                return 0;
            }
            GrantAuthority authority(state, config->state_root / "grants");
            if (mode == "--grant-issue")
                std::cout << Json{{"grant_id", authority.issue(grant_definition(
                                                   read_json(path_from_utf8(args[1]), 65536)))}}
                                 .dump()
                          << '\n';
            else if (mode == "--grant-revoke") {
                authority.revoke(args[1]);
                std::cout << authority.inspect(args[1]).dump() << '\n';
            } else if (mode == "--grant-inspect")
                std::cout << authority.inspect(args[1]).dump() << '\n';
            else {
                if (config->runtime_mode != RuntimeMode::host || !config->host_exec_enabled)
                    throw Error("Granted program execution requires the enabled host runtime");
                const auto request = read_json(path_from_utf8(args[1]), 65536);
                std::cout << execute_granted_program(
                                 authority, private_root, json_string(request, "grant_id"),
                                 {json_string(request, "principal"), json_string(request, "run"),
                                  json_string(request, "operation")},
                                 request.at("arguments"))
                                 .dump()
                          << '\n';
            }
            return 0;
        }
        if (mode == "--migrate-state") {
            if (args.size() != 1)
                throw Error("--migrate-state does not accept request payloads");
            std::cout << migrate_legacy_state(config).dump(2) << '\n';
            return 0;
        }
        if (mode == "--job-runner") {
            if (args.size() != 2)
                throw Error("--job-runner requires exactly one request.json path");
            return run_job_request(config, path_from_utf8(args[1]));
        }
        if (mode == "--dump-contract") {
            std::cout << ToolContract(*config).all().dump(2) << '\n';
            return 0;
        }
        if (!mode.empty() && mode != "--parity-report")
            throw Error("Unknown command: " + mode);
        auto engine = std::make_shared<Engine>(config);
        if (mode == "--parity-report") {
            std::cout << engine->parity_report().dump(2) << '\n';
            return 0;
        }
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
#ifdef _WIN32
        SetConsoleCtrlHandler(console_handler, TRUE);
#endif
        HttpServer server(config, engine);
        const auto port = server.start();
        engine->attach(server);
        std::cout << "C++ Devbox MCP listening on " << config->host << ':' << port << '\n' << std::flush;
#ifdef _WIN32
        LONG running = 0;
        while (InterlockedCompareExchange(&shutdown_requested, 0, 0) == 0)
            if (!WaitOnAddress(&shutdown_requested, &running, sizeof(running), INFINITE))
                throw Error("wait for shutdown: " + windows_error());
#else
        while (!shutdown_requested)
            std::this_thread::sleep_for(Millis(50));
#endif
        server.stop();
        engine->stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Devbox C++: " << e.what() << '\n';
        return 1;
    }
}
#ifndef DEVBOX_NO_MAIN
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(devbox::narrow(argv[i]));
    return devbox::run_mcp(args);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return devbox::run_mcp(args);
}
#endif
#endif
