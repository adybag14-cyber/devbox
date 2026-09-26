#include "devbox/result.hpp"
#include "devbox/server.hpp"
#include "devbox/state_coordinator.hpp"
#include <future>
#include <iostream>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
struct Impostor : McpBackend {
    std::mutex mutex;
    std::string captured;
    std::string upstream;
    std::atomic_bool forward{false};
    std::atomic_uint lose_replies{0}, forwarded{0}, dropped{0};
    Json server_info() const override {
        return Json{{"name", "owned impostor fixture"}, {"version", "1"}};
    }
    Json list_tools(std::string_view) const override {
        return Json::array({Json{{"name", "devbox_internal_state"}}});
    }
    bool ready() const override {
        return true;
    }
    asio::awaitable<Json> metadata(const HttpRequest&) override {
        co_return server_info();
    }
    void observe_http(const HttpRequest&, int, std::uint64_t, Millis, bool) override {}
    asio::awaitable<Json> call_tool(std::string, Json args, Cancel) override {
        {
            std::lock_guard lock(mutex);
            captured = args.dump();
        }
        if (forward.load()) {
            const auto body = Json{{"jsonrpc", "2.0"},
                                   {"id", args.at("nonce")},
                                   {"method", "tools/call"},
                                   {"params", {{"name", "devbox_internal_state"}, {"arguments", args}}}}
                                  .dump();
            const auto result =
                http_request("POST", upstream, body,
                             Json{{"content-type", "application/json"}, {"accept", "application/json"}},
                             Millis(2000), 65536, {}, true);
            ++forwarded;
            auto remaining = lose_replies.load();
            while (remaining && !lose_replies.compare_exchange_weak(remaining, remaining - 1)) {
            }
            if (!remaining)
                co_return Json::parse(result.body).at("result");
            ++dropped;
            // The real transaction has committed, but the client loses its
            // authenticated acknowledgement. Never return that reply as success.
        }
        co_return result_success("fake", Json{{"nonce", args.at("nonce")},
                                              {"generation", args.at("generation")},
                                              {"sealed", "invalid"},
                                              {"mac", std::string(64, '0')}});
    }
};
int run(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator") {
        try {
            return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
        } catch (...) {
            return 2;
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "--frontend") {
        auto store = open_coordinated_state(path_from_utf8(argv[2]));
        StateMutation value{{"run", "survives", "owner", "scope", "created", 0, Json{{"alive", true}}}, 0};
        store->apply_once("frontend-create", {&value, 1});
        return 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-state-ipc-" + uuid());
    ensure_directory(root);
    const auto directory = root / "state";
    ScopeExit cleanup([&] {
        if (stop_state_coordinator(directory)) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    });
    try {
        ProcessOptions frontend;
        frontend.allow_durable_children = true;
        frontend.timeout = Millis(15000);
        spawn_process(path_text(executable_path()), {"--frontend", path_text(directory)}, frontend);
        auto one = open_coordinated_state(directory), two = open_coordinated_state(directory);
        require(one->get("run", "survives").has_value(),
                "coordinator and acknowledged state survive frontend exit");
        const auto generation = one->generation();
        StateMutation value{
            {"run", "shared", "owner", "scope", "created", 0, Json{{"text", "CONFIDENTIAL_STATE_PAYLOAD"}}},
            0};
        auto a = std::async(std::launch::async, [&] { return one->apply_once("same-batch", {&value, 1}); });
        auto b = std::async(std::launch::async, [&] { return two->apply_once("same-batch", {&value, 1}); });
        require(a.get() != b.get() && one->count("run") == 2,
                "concurrent clients share one writer and one admission receipt");
        const auto descriptor = read_json(directory / "coordinator.json");
        const auto key = json_string(descriptor, "key");
        const auto raw = http_request(
            "POST", "http://127.0.0.1:" + std::to_string(json_uint(descriptor, "port")) + "/mcp",
            Json{{"jsonrpc", "2.0"},
                 {"id", 1},
                 {"method", "tools/call"},
                 {"params",
                  {{"name", "devbox_internal_state"}, {"arguments", {{"mac", std::string(64, '0')}}}}}}
                .dump(),
            Json{{"content-type", "application/json"}, {"accept", "application/json"}}, Millis(1000), 65536,
            {}, true);
        require(Json::parse(raw.body)["result"]["isError"] == true && raw.body.find(key) == std::string::npos,
                "unauthenticated local callers receive neither data nor credentials");
        {
            const auto prior = environment("HTTP_PROXY"), prior_no = environment("NO_PROXY");
            ScopeExit restore([&] {
                set_environment("HTTP_PROXY", prior);
                set_environment("NO_PROXY", prior_no);
            });
            set_environment("HTTP_PROXY", "http://127.0.0.1:1");
            set_environment("NO_PROXY", "");
            require(two->get("run", "shared")->data["text"] == "CONFIDENTIAL_STATE_PAYLOAD",
                    "private IPC never uses an ambient HTTP proxy");
        }
        {
            auto config = std::make_shared<Config>();
            config->host = "127.0.0.1";
            config->port = 0;
            config->project_root = root;
            auto impostor = std::make_shared<Impostor>();
            impostor->upstream = "http://127.0.0.1:" + std::to_string(json_uint(descriptor, "port")) + "/mcp";
            impostor->forward = true;
            HttpServer fake(config, impostor);
            const auto port = fake.start();
            auto redirected = descriptor;
            redirected["port"] = port;
            write_json_atomic(directory / "coordinator.json", redirected);
            ScopeExit restore([&] { write_json_atomic(directory / "coordinator.json", descriptor); });
            StateClientOptions options;
            options.start_if_absent = false;
            auto client = open_coordinated_state(directory, options);
            require(client->get("run", "shared")->data["text"] == "CONFIDENTIAL_STATE_PAYLOAD",
                    "forwarded authenticated response warms a private connection");
            StateMutation lost{{"run", "lost-ack", "owner", "scope", "committed", 0,
                                Json{{"effect", "recorded exactly once"}}},
                               0};
            StateEvent observed{"lost-ack-run", "committed", 0, Json{{"effect", 1}}};
            const auto first_count = impostor->forwarded.load();
            impostor->lose_replies = 1;
            require(client->apply_once("lost-ack-batch", {&lost, 1}, {&observed, 1}),
                    "single lost acknowledgement recovers through a verified replay receipt");
            require(impostor->forwarded == first_count + 2 && impostor->dropped == 1,
                    "recovery sends exactly one new authenticated request, not a third attempt");
            require(one->get("run", "lost-ack")->revision == 1 &&
                        one->events("lost-ack-run", 0, 10).size() == 1,
                    "lost-acknowledgement retry preserves exactly one revision and event");
            require(client->apply_once("lost-ack-batch", {&lost, 1}, {&observed, 1}),
                    "later explicit replay retains the same original receipt");
            lost.record.id = "lost-ack-twice";
            observed.run = "lost-ack-twice-run";
            const auto second_count = impostor->forwarded.load();
            impostor->lose_replies = 2;
            bool unconfirmed = false;
            try {
                (void)client->apply_once("lost-ack-twice-batch", {&lost, 1}, {&observed, 1});
            } catch (const Error& error) {
                unconfirmed = std::string_view(error.what()).starts_with("STATE_IPC_OUTCOME_UNCONFIRMED");
            }
            require(unconfirmed && impostor->forwarded == second_count + 2 && impostor->dropped == 3,
                    "two missing proofs exhaust the existing bound without claiming success");
            require(one->get("run", "lost-ack-twice")->revision == 1 &&
                        one->events("lost-ack-twice-run", 0, 10).size() == 1,
                    "unknown acknowledgement does not undo or duplicate the committed transaction");
            require(client->apply_once("lost-ack-twice-batch", {&lost, 1}, {&observed, 1}),
                    "retry after exhausted acknowledgement budget uses the original batch identity");
            require(one->get("run", "lost-ack-twice")->revision == 1 &&
                        one->events("lost-ack-twice-run", 0, 10).size() == 1,
                    "explicit retry after lost acknowledgements cannot repeat mutation or event");
            // The next response travels over the already warmed channel. A
            // prior proof must never turn that channel into cached authority.
            impostor->forward = false;
            bool denied = false;
            try {
                client->apply_once("impostor", {&value, 1});
            } catch (const Error&) {
                denied = true;
            }
            require(denied, "server proof prevents accepting an impostor acknowledgement");
            {
                std::lock_guard lock(impostor->mutex);
                require(!impostor->captured.empty() &&
                            impostor->captured.find("CONFIDENTIAL_STATE_PAYLOAD") == std::string::npos &&
                            impostor->captured.find(key) == std::string::npos,
                        "intercepted IPC discloses neither state payload nor channel "
                        "credentials");
            }
            fake.stop();
        }
        require(stop_state_coordinator(directory), "owned coordinator stops through authenticated control");
        require(one->get("run", "shared")->revision == 1 && one->generation() > generation,
                "client reconnect restarts the writer with a newer fence and "
                "retained data");
        require(two->apply_once("same-batch", {&value, 1}),
                "cross-restart receipt replay cannot duplicate an admitted "
                "transaction");
        require(stop_state_coordinator(directory), "restarted coordinator reaches terminal stop");
        std::cout << "Encrypted authenticated IPC, single writer, frontend "
                     "survival, spoof refusal and "
                     "restart replay passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_args) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(narrow(wide_args[i]));
    std::vector<char*> values;
    for (auto& value : args)
        values.push_back(value.data());
    return run(argc, values.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
