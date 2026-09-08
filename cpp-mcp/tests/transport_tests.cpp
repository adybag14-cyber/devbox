#include "devbox/result.hpp"
#include "devbox/server.hpp"
#include <boost/beast.hpp>
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;
namespace http = boost::beast::http;
using Tcp = asio::ip::tcp;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
struct FixtureBackend : McpBackend {
    WorkPool workers{2, 2};
    std::atomic_size_t entered{0}, cancelled{0}, observations{0}, disconnects{0};
    Json server_info() const override {
        return Json{{"name", "C++ transport test fixture"}, {"version", "1"}};
    }
    Json list_tools(std::string_view) const override {
        Json result = Json::array();
        for (const auto name : {"devbox_wait", "host_exec"})
            result.push_back(
                Json{{"name", name},
                     {"description", "Transport verification fixture"},
                     {"inputSchema",
                      {{"type", "object"},
                       {"properties",
                        {{"delay_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}}}}}}});
        return result;
    }
    asio::awaitable<Json> call_tool(std::string name, Json arguments, Cancel cancel) override {
        ++entered;
        try {
            const auto delay = Millis(json_uint(arguments, "delay_ms"));
            if (name == "devbox_wait")
                co_await async_delay(delay, cancel);
            else if (name == "host_exec") {
                auto pending = workers.run(
                    [delay, cancel] {
                        const auto deadline = Clock::now() + delay;
                        while (Clock::now() < deadline) {
                            if (cancel->wait_for(Millis(10)))
                                throw Cancelled();
                        }
                        cancel->check();
                    },
                    cancel);
                co_await std::move(pending);
            } else
                throw Error("Unknown fixture tool");
        } catch (const Cancelled&) {
            ++cancelled;
            throw;
        }
        co_return result_success("fixture complete", Json{{"name", name}});
    }
    asio::awaitable<Json> metadata(const HttpRequest&) override {
        co_return Json{{"name", "fixture"}};
    }
    bool ready() const override {
        return true;
    }
    void observe_http(const HttpRequest&, int, std::uint64_t, Millis, bool disconnected) override {
        ++observations;
        if (disconnected)
            ++disconnects;
    }
};
Json headers(std::string accept = "application/json") {
    return Json{
        {"content-type", "application/json"}, {"accept", accept}, {"user-agent", "transport-fixture"}};
}
Json rpc(std::string method, Json id = 1, Json params = Json::object()) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
Json call(std::string name, std::uint64_t delay = 0, Json id = 1) {
    return rpc("tools/call", id, Json{{"name", name}, {"arguments", {{"delay_ms", delay}}}});
}
std::unique_ptr<Tcp::socket> raw_request(asio::io_context& io, std::uint16_t port, const Json& body,
                                         const Json& supplied_headers) {
    auto socket = std::make_unique<Tcp::socket>(io);
    socket->connect({asio::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set(http::field::host, "127.0.0.1");
    for (auto it = supplied_headers.begin(); it != supplied_headers.end(); ++it)
        request.set(it.key(), it.value().get<std::string>());
    request.body() = body.dump();
    request.prepare_payload();
    http::write(*socket, request);
    return socket;
}
template <class Predicate> void until(Predicate predicate, const char* description) {
    const auto deadline = Clock::now() + Millis(3000);
    while (!predicate() && Clock::now() < deadline)
        std::this_thread::sleep_for(Millis(5));
    require(predicate(), description);
}
int main(int argc, char** argv) {
    const auto root = argc == 3 && std::string(argv[1]) == "--serve"
                          ? path_from_utf8(argv[2])
                          : fs::temp_directory_path() / ("devbox-cpp-transport-" + uuid());
    try {
        fs::create_directories(root);
        auto config = std::make_shared<Config>();
        config->host = "127.0.0.1";
        config->port = 0;
        config->project_root = root;
        config->oauth_state_file_path = root / "oauth.json";
        config->gateway_bridge_enabled = true;
        config->gateway_bridge_origins = {"https://chatgpt.com"};
        config->mcp_json_body_limit_bytes = 4096;
        auto backend = std::make_shared<FixtureBackend>();
        HttpServer server(config, backend);
        const auto port = server.start();
        const auto base = "http://127.0.0.1:" + std::to_string(port);
        if (argc == 3 && std::string(argv[1]) == "--serve") {
            write_json_atomic(root / "ready.json", Json{{"port", port}, {"pid", process_id()}});
            while (!fs::exists(root / "stop"))
                std::this_thread::sleep_for(Millis(25));
            server.stop();
            return 0;
        }
        require(http_request("GET", base + "/healthz").body == "ok", "health probe");
        require(http_request("GET", base + "/readyz").status == 200, "readiness probe");
        const auto initialize =
            http_request("POST", base + "/mcp",
                         rpc("initialize", 0, Json{{"protocolVersion", "2025-11-25"}}).dump(), headers());
        require(Json::parse(initialize.body)["result"]["protocolVersion"] == "2025-11-25",
                "legacy SDK handshake");
        const auto initialize_sse = http_request(
            "POST", base + "/mcp", rpc("initialize", 1, Json{{"protocolVersion", "2025-06-18"}}).dump(),
            headers("application/json, text/event-stream"));
        require(json_string(initialize_sse.headers, "content-type").starts_with("text/event-stream") &&
                    initialize_sse.body.find("event: message\ndata:") != initialize_sse.body.npos &&
                    initialize_sse.body.find("2025-06-18") != initialize_sse.body.npos,
                "legacy initialize negotiates the requested SSE transport");
        auto notify = rpc("notifications/initialized");
        notify.erase("id");
        require(http_request("POST", base + "/mcp", notify.dump(), headers()).status == 202,
                "initialized notification");
        auto cancelled_stream = std::async(std::launch::async, [&] {
            return http_request("POST", base + "/mcp", call("devbox_wait", 10000, "stream-cancel").dump(),
                                headers("application/json, text/event-stream"), Millis(2500));
        });
        until([&] { return server.active_requests() == 1; }, "SSE cancellation target registered");
        auto cancel_stream = rpc("notifications/cancelled", 0, Json{{"requestId", "stream-cancel"}});
        cancel_stream.erase("id");
        require(http_request("POST", base + "/mcp", cancel_stream.dump(), headers()).status == 202,
                "SSE cancellation notification accepted");
        const auto cancelled_response = cancelled_stream.get();
        require(cancelled_response.body.find("event: message") != std::string::npos &&
                    cancelled_response.body.find("-32800") != std::string::npos,
                "explicit cancellation returns a terminal SSE response to the connected client");
        require(Json::parse(http_request("POST", base + "/mcp", rpc("tools/list").dump(), headers())
                                .body)["result"]["tools"]
                        .size() == 2,
                "schema listing");
        const auto sse = http_request("POST", base + "/mcp", call("devbox_wait", 1100).dump(),
                                      headers("application/json, text/event-stream"), Millis(3000));
        require(sse.status == 200 && sse.body.find(": heartbeat") != sse.body.npos &&
                    sse.body.find("event: message") != sse.body.npos &&
                    sse.body.find("fixture complete") != sse.body.npos,
                "per-request SSE and heartbeat");
        auto version = headers();
        version["mcp-protocol-version"] = "2026-07-28";
        require(http_request("POST", base, rpc("tools/list").dump(), version).status == 400,
                "new revision requires per-request lifecycle metadata");
        version["mcp-method"] = "tools/list";
        const Json meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                        {"io.modelcontextprotocol/clientCapabilities", Json::object()}};
        const auto modern_list =
            http_request("POST", base, rpc("tools/list", 1, Json{{"_meta", meta}}).dump(), version);
        const auto modern_result = Json::parse(modern_list.body)["result"];
        require(modern_list.status == 200 && modern_result["resultType"] == "complete" &&
                    modern_result["ttlMs"] == 0 && modern_result["cacheScope"] == "public" &&
                    modern_result["tools"].size() == 2,
                "new revision complete result and tool cache hints");
        version["mcp-method"] = "tools/call";
        const Json modern_call{{"name", "devbox_wait"}, {"arguments", Json::object()}, {"_meta", meta}};
        require(http_request("POST", base, rpc("tools/call", 1, modern_call).dump(), version).status == 400,
                "missing mirrored tool name rejected before execution");
        version["mcp-name"] = "devbox_wait";
        const auto modern_tool =
            http_request("POST", base, rpc("tools/call", 1, modern_call).dump(), version);
        require(modern_tool.status == 200 &&
                    Json::parse(modern_tool.body)["result"]["resultType"] == "complete",
                "modern tool response discriminator");
        version["mcp-method"] = "ping";
        require(http_request("POST", base, rpc("ping", 1, Json{{"_meta", meta}}).dump(), version).status ==
                    404,
                "legacy ping is not a modern protocol method");
        version["mcp-protocol-version"] = "unknown";
        require(http_request("POST", base, rpc("ping").dump(), version).status == 400,
                "unsupported revision rejected");
        require(http_request("POST", base, "{invalid", headers()).status == 400, "malformed JSON rejected");
        require(http_request("POST", base, std::string(4097, 'x'), headers()).status == 413,
                "bounded HTTP body");
        auto invalid_host = headers();
        invalid_host["host"] = "attacker.invalid";
        require(http_request("GET", base + "/healthz", {}, invalid_host).status == 403,
                "Host rebinding defense");
        auto bridge = headers();
        bridge["origin"] = "https://chatgpt.com";
        bridge["access-control-request-private-network"] = "true";
        const auto preflight = http_request("OPTIONS", base, {}, bridge);
        require(preflight.status == 204 &&
                    preflight.headers["access-control-allow-origin"] == "https://chatgpt.com" &&
                    preflight.headers["access-control-allow-private-network"] == "true",
                "local bridge preflight");
        bridge["x-forwarded-for"] = "203.0.113.4";
        require(http_request("OPTIONS", base, {}, bridge).status == 405,
                "forwarded remote requests cannot expose local bridge");
        bridge.erase("x-forwarded-for");
        bridge["origin"] = "https://attacker.invalid";
        require(http_request("OPTIONS", base, {}, bridge).status == 403, "origin allowlist");
        asio::io_context io;
        const auto before_cancel = backend->cancelled.load();
        auto abandoned =
            raw_request(io, port, call("devbox_wait", 10000, "abandoned"), headers("text/event-stream"));
        boost::beast::flat_buffer response_buffer;
        http::response_parser<http::empty_body> response_parser;
        http::read_header(*abandoned, response_buffer, response_parser);
        until([&] { return server.active_requests() == 1; }, "abandoned call registered");
        abandoned->close();
        until([&] { return backend->cancelled > before_cancel && server.active_requests() == 0; },
              "client disconnect cancels native coroutine");
        auto first = raw_request(io, port, call("devbox_wait", 10000, 17), headers());
        until([&] { return server.active_requests() == 1; }, "notification target registered");
        auto cancellation = rpc("notifications/cancelled", 0, Json{{"requestId", 17}});
        cancellation.erase("id");
        auto wrong_agent = headers();
        wrong_agent["user-agent"] = "unrelated-client";
        require(http_request("POST", base, cancellation.dump(), wrong_agent).status == 202,
                "unrelated cancellation acknowledged");
        std::this_thread::sleep_for(Millis(80));
        require(server.active_requests() == 1, "cancellation isolated by request identity");
        auto wrong_id = cancellation;
        wrong_id["params"]["requestId"] = "17";
        http_request("POST", base, wrong_id.dump(), headers());
        std::this_thread::sleep_for(Millis(40));
        require(server.active_requests() == 1, "string and numeric request IDs differ");
        http_request("POST", base, cancellation.dump(), headers());
        until([&] { return server.active_requests() == 0; }, "authorized cancellation delivered");
        first->close();
        std::vector<std::unique_ptr<Tcp::socket>> pending;
        for (int i = 0; i < 30; ++i)
            pending.push_back(raw_request(io, port, call("devbox_wait", 10000, 100 + i), headers()));
        until([&] { return server.active_requests() == 30; }, "passive wait fanout");
        const auto health_start = Clock::now();
        require(http_request("GET", base + "/healthz", {}, Json::object(), Millis(500)).body == "ok" &&
                    Clock::now() - health_start < Millis(500),
                "passive waits do not consume command workers or health threads");
        for (auto& socket : pending)
            socket->close();
        pending.clear();
        until([&] { return server.active_requests() == 0; }, "fanout cancellation clears registry");
        for (int i = 0; i < 5; ++i)
            pending.push_back(raw_request(io, port, call("host_exec", 10000, 200 + i), headers()));
        until([&] { return server.active_requests() >= 4; }, "bounded work pool active and queued calls");
        require(http_request("GET", base + "/healthz", {}, Json::object(), Millis(500)).status == 200,
                "blocking pool saturation preserves health");
        for (auto& socket : pending)
            socket->close();
        pending.clear();
        until([&] { return server.active_requests() == 0; }, "queued and active work cancellation");
        bool exclusive = false;
        auto collision_config = std::make_shared<Config>(*config);
        collision_config->port = port;
        try {
            HttpServer collision(collision_config, backend);
            collision.start();
        } catch (...) {
            exclusive = true;
        }
        require(exclusive, "listener cannot displace an existing server");
        server.stop();
        auto oauth_config = std::make_shared<Config>(*config);
        oauth_config->auth_mode = AuthMode::demo_oauth;
        oauth_config->public_base_url = "http://localhost";
        HttpServer protected_server(oauth_config, backend);
        const auto oauth_port = protected_server.start();
        const auto oauth_base = "http://127.0.0.1:" + std::to_string(oauth_port);
        const auto challenge = http_request("POST", oauth_base + "/mcp", rpc("ping").dump(), headers());
        require(challenge.status == 401 &&
                    json_string(challenge.headers, "www-authenticate").find("resource_metadata") !=
                        std::string::npos,
                "OAuth bearer challenge");
        require(http_request("GET", oauth_base + "/.well-known/oauth-authorization-server").status == 200,
                "OAuth discovery route");
        const auto client = Json::parse(http_request("POST", oauth_base + "/register",
                                                     Json{{"redirect_uris", {"http://127.0.0.1/callback"}},
                                                          {"token_endpoint_auth_method", "none"}}
                                                         .dump(),
                                                     headers())
                                            .body);
        // Seed through the same persisted state shape to keep the HTTP scope test independent of
        // redirect-following client behavior.
        auto state = read_json(oauth_config->oauth_state_file_path);
        state["accessTokens"].push_back(Json::array({"narrow", Json{{"clientId", client["client_id"]},
                                                                    {"scopes", {"mcp:devbox:read"}},
                                                                    {"expiresAt", unix_millis() + 300000}}}));
        protected_server.stop();
        write_json_atomic(oauth_config->oauth_state_file_path, state);
        HttpServer reopened(oauth_config, backend);
        const auto reopened_base = "http://127.0.0.1:" + std::to_string(reopened.start());
        auto authorized = headers();
        authorized["authorization"] = "Bearer narrow";
        const auto denied =
            Json::parse(http_request("POST", reopened_base, call("host_exec").dump(), authorized).body);
        require(denied.contains("result") && denied["result"]["isError"] == true &&
                    denied["result"]["structuredContent"]["data"]["requiredScope"] == "mcp:host:exec",
                "HTTP tool scope enforcement");
        require(Json::parse(http_request("POST", reopened_base, call("devbox_wait").dump(), authorized).body)
                    .contains("result"),
                "HTTP narrow read scope works");
        bridge["origin"] = "https://chatgpt.com";
        require(http_request("OPTIONS", reopened_base, {}, bridge).status == 405,
                "OAuth disables unauthenticated local bridge");
        reopened.stop();
        fs::remove_all(root);
        std::cout
            << "HTTP/SSE, MCP, Host/CORS, scoped cancellation, bounded workers and OAuth routes passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
