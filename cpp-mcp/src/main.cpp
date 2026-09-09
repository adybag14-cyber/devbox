#include "devbox/engine.hpp"
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
    try {
        const auto mode = args.empty() ? "" : args.front();
        if (mode == "--capture-worker")
            return run_capture_worker(std::vector<std::string>(args.begin() + 1, args.end()));
        if (mode == "--help" || mode == "-h") {
            std::cout
                << "Devbox C++ MCP " << build_version()
                << "\nUsage: devbox-mcp [--build-info|--parity-report|--dump-contract|--job-runner "
                   "PATH|--elevated-shell-worker PATH|--capture-worker OUTPUT MODE QUALITY [PID TREE]]\n";
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
        auto config = std::make_shared<Config>(Config::load());
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
