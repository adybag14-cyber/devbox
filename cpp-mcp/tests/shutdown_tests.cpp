#include "../src/server_main.hpp"
#include "devbox/engine.hpp"
#include <csignal>
#include <future>
#include <iostream>
using namespace devbox;
int main(int argc, char** argv) {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-shutdown-" + uuid());
    try {
        fs::create_directory(root);
        ScopeExit remove([&] {
            std::error_code error;
            fs::remove_all(root, error);
        });
        asio::io_context io;
        asio::ip::tcp::acceptor reserve(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto port = reserve.local_endpoint().port();
        reserve.close();
        const auto base = "http://127.0.0.1:" + std::to_string(port);
        for (const auto& [name, value] :
             {std::pair{"DEVBOX_PROJECT_ROOT", path_text(root)},
              std::pair{"HOST_WORKSPACE_PATH", path_text(root)},
              std::pair{"HOST_DEFAULT_WORKDIR", path_text(root)},
              std::pair{"DEVBOX_WORKSPACE_PATH", path_text(root)},
              std::pair{"MCP_JOBS_ROOT", path_text(root / "jobs")},
              std::pair{"MCP_EXEC_SLOT_ROOT", path_text(root / "slots")},
              std::pair{"MCP_PERFORMANCE_STATE_PATH", path_text(root / "run" / "performance.json")},
              std::pair{"OAUTH_STATE_FILE_PATH", path_text(root / "oauth.json")},
              std::pair{"PORT", std::to_string(port)}})
            set_environment(name, value);
        for (const auto& [name, value] :
             {std::pair{"HOST", "127.0.0.1"}, std::pair{"MCP_AUTH_MODE", "none"},
              std::pair{"PUBLIC_BASE_URL", ""}, std::pair{"DEVBOX_RUNTIME_MODE", "host"},
              std::pair{"DEVBOX_AUTO_START", "false"}, std::pair{"ENABLE_GATEWAY_BRIDGE", "false"},
              std::pair{"DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE", "1"}})
            set_environment(name, value);
        const auto signal = argc == 2 && std::string_view(argv[1]) == "interrupt" ? SIGINT : SIGTERM;
        auto stop = std::async(std::launch::async, [base, signal] {
            bool ready = false;
            const auto deadline = Clock::now() + Millis(10000);
            while (!ready && Clock::now() < deadline) {
                try {
                    ready =
                        http_request("GET", base + "/readyz", {}, Json::object(), Millis(200)).status == 200;
                } catch (...) {
                }
                if (!ready)
                    std::this_thread::sleep_for(Millis(10));
            }
            // raise targets this test process. It exercises the real signal
            // handler, main wait, server stop, background joins and log drain.
            const auto started = Clock::now();
            const auto raised = std::raise(signal);
            return std::tuple{ready, raised, started};
        });
        const auto exit = run_mcp({});
        const auto [ready, raised, stopped_at] = stop.get();
        if (!ready || raised != 0 || exit != 0 || Clock::now() - stopped_at > Millis(3000))
            throw Error("Real runtime failed readiness or prompt graceful signal shutdown");
        std::cout << "Real runtime signal shutdown completed and joined its workers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
