#include "devbox/native.hpp"
#include "devbox/result.hpp"
#include "devbox/server.hpp"
#include <boost/beast.hpp>
#include <future>
#include <iostream>
#include <set>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F fn, std::string_view expected) {
    try {
        fn();
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos, "unexpected error");
        return;
    }
    throw Error("expected local HTTP rejection");
}
struct Fixture final : McpBackend {
    std::mutex mutex;
    std::map<std::string, Json> traces;
    std::atomic_uint calls{0};
    Json server_info() const override {
        return Json{{"name", "owned connection reuse fixture"}, {"version", "1"}};
    }
    Json list_tools(std::string_view) const override {
        return Json::array({Json{{"name", "echo"}}});
    }
    bool ready() const override {
        return true;
    }
    asio::awaitable<Json> metadata(const HttpRequest&) override {
        co_return server_info();
    }
    asio::awaitable<Json> call_tool(std::string, Json args, Cancel cancel) override {
        ++calls;
        if (json_uint(args, "delay"))
            co_await async_delay(Millis(json_uint(args, "delay")), cancel);
        co_return result_success("echo", args);
    }
    void observe_http(const HttpRequest& r, int, std::uint64_t, Millis, bool) override {
        std::lock_guard lock(mutex);
        traces[json_string(r.headers, "x-fixture-id")] =
            Json{{"connection", r.connection_created_at}, {"previous", json_string(r.headers, "x-previous")}};
    }
    Json trace(const std::string& id) {
        const auto deadline = Clock::now() + Millis(3000);
        while (Clock::now() < deadline) {
            {
                std::lock_guard lock(mutex);
                if (traces.contains(id))
                    return traces.at(id);
            }
            std::this_thread::sleep_for(Millis(1));
        }
        throw Error("missing bounded fixture trace");
    }
};
Json request(const std::string& id, const Json& args) {
    return Json{{"jsonrpc", "2.0"},
                {"id", id},
                {"method", "tools/call"},
                {"params", {{"name", "echo"}, {"arguments", args}}}};
}
} // namespace
int main() {
    try {
        const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-local-http-" + uuid());
        ensure_directory(root);
        ScopeExit cleanup([&] {
            std::error_code ec;
            fs::remove_all(root, ec);
        });
        auto config = std::make_shared<Config>();
        config->host = "127.0.0.1";
        config->port = 0;
        config->project_root = root;
        auto backend = std::make_shared<Fixture>();
        HttpServer server(config, backend);
        const auto url = "http://127.0.0.1:" + std::to_string(server.start()) + "/mcp";
        auto invoke = [&](const std::string& id, std::string_view scope, Json args = Json::object(),
                          Json extra = Json::object(), std::size_t maximum = 65536,
                          Millis timeout = Millis(3000), Cancel cancel = {}) {
            Json headers{
                {"content-type", "application/json"}, {"accept", "application/json"}, {"x-fixture-id", id}};
            headers.update(extra);
            return http_request("POST", url, request(id, args).dump(), headers, timeout, maximum, cancel,
                                true, scope);
        };
        auto expect = [&](const std::string& id, std::string_view scope, const Json& args,
                          Json extra = Json::object()) {
            auto response = invoke(id, scope, args, std::move(extra));
            require(response.status == 200, "HTTP status");
            const auto value = Json::parse(response.body);
            require(value["id"] == id && value["result"]["structuredContent"]["data"] == args,
                    "fresh body and identity");
            return backend->trace(id);
        };
        std::set<std::string> fresh, reused;
        for (unsigned i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(Millis(5));
            fresh.insert(expect("fresh-" + std::to_string(i), {}, Json{{"i", i}})["connection"]);
        }
        require(fresh.size() == 5, "fresh comparator must create five connections");
        for (unsigned i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(Millis(5));
            reused.insert(expect("reuse-" + std::to_string(i), "instance-a", Json{{"i", i}})["connection"]);
        }
        require(reused.size() == 1, "read session must reuse one connection");
        auto first = expect("header-one", "instance-a", Json{{"payload", "old"}},
                            Json{{"x-previous", "must-not-leak"}});
        auto second = expect("header-two", "instance-a", Json{{"payload", "new"}});
        require(first["connection"] == second["connection"] && first["previous"] == "must-not-leak" &&
                    second["previous"] == "",
                "headers and bodies rebound on reused transport");
        std::this_thread::sleep_for(Millis(5));
        auto changed = expect("new-scope", "instance-b", Json::object());
        require(changed["connection"] != second["connection"], "new coordinator instance closes old session");
        auto closed = expect("close", "instance-b", Json::object(), Json{{"connection", "close"}});
        std::this_thread::sleep_for(Millis(5));
        auto reopened = expect("reopen", "instance-b", Json::object());
        require(closed["connection"] != reopened["connection"], "closed peer reconnects safely");
        auto token = std::make_shared<Cancellation>();
        token->cancel();
        const auto calls = backend->calls.load();
        rejects(
            [&] {
                invoke("pre-cancel", "instance-b", Json::object(), Json::object(), 65536, Millis(3000),
                       token);
            },
            "cancel");
        require(backend->calls == calls, "pre-cancel does not dispatch");
        rejects(
            [&] {
                invoke("oversize", "instance-b", Json{{"payload", std::string(4096, 'x')}}, Json::object(),
                       32);
            },
            "byte limit");
        std::this_thread::sleep_for(Millis(5));
        auto after_size = expect("after-size", "instance-b", Json{{"small", true}});
        require(after_size["connection"] != reopened["connection"], "oversize closes reusable connection");
        rejects(
            [&] {
                invoke("timeout", "instance-b", Json{{"delay", 2000}}, Json::object(), 65536, Millis(50));
            },
            "HTTP request failed");
        std::this_thread::sleep_for(Millis(5));
        auto after_timeout = expect("after-timeout", "instance-b", Json::object());
        require(after_timeout["connection"] != after_size["connection"], "timeout discards reusable handle");
        rejects(
            [&] {
                http_request("POST", "http://localhost:1/mcp", "", {}, Millis(10), 16, {}, true, "scope");
            },
            "SCOPE_INVALID");
        rejects(
            [&] {
                http_request("POST", "http://127.0.0.1:1/other", "", {}, Millis(10), 16, {}, true, "scope");
            },
            "SCOPE_INVALID");
        rejects([&] { http_request("POST", url, "", {}, Millis(10), 16, {}, false, "scope"); },
                "SCOPE_INVALID");
        rejects([&] { http_request("GET", url, "", {}, Millis(10), 16, {}, true, "scope"); },
                "SCOPE_INVALID");
        rejects(
            [&] {
                http_request("POST", "http://127.0.0.1:65536/mcp", "", {}, Millis(10), 16, {}, true, "scope");
            },
            "SCOPE_INVALID");
        {
            // A redirect must not send the sealed request to a different endpoint.
            asio::io_context io;
            asio::ip::tcp::acceptor redirect(io, {asio::ip::make_address("127.0.0.1"), 0});
            const auto redirect_port = redirect.local_endpoint().port();
            std::thread peer([&] {
                asio::ip::tcp::socket socket(io);
                redirect.accept(socket);
                boost::beast::flat_buffer buffer;
                boost::beast::http::request<boost::beast::http::string_body> input;
                boost::beast::http::read(socket, buffer, input);
                boost::beast::http::response<boost::beast::http::string_body> response{
                    boost::beast::http::status::found, 11};
                response.set(boost::beast::http::field::location, url);
                response.keep_alive(false);
                response.body() = "do not follow";
                response.prepare_payload();
                boost::beast::http::write(socket, response);
            });
            ScopeExit join([&] { peer.join(); });
            const auto before = backend->calls.load();
            auto response = http_request("POST", "http://127.0.0.1:" + std::to_string(redirect_port) + "/mcp",
                                         "sealed-fixture", {}, Millis(1000), 1024, {}, true, "redirect");
            require(response.status == 302 && backend->calls == before,
                    "local reuse cannot follow redirects");
        }
        {
            auto other_backend = std::make_shared<Fixture>();
            HttpServer other(config, other_backend);
            const auto other_url = "http://127.0.0.1:" + std::to_string(other.start()) + "/mcp";
            const auto before = backend->calls.load();
            const auto response = http_request("POST", other_url, request("other", Json::object()).dump(),
                                               Json{{"content-type", "application/json"}}, Millis(3000),
                                               65536, {}, true, "instance-b");
            require(response.status == 200 && other_backend->calls == 1 && backend->calls == before,
                    "port switch cannot retain previous destination");
            other.stop();
        }
        std::vector<std::future<void>> threads;
        for (unsigned t = 0; t < 4; ++t)
            threads.push_back(std::async(std::launch::async, [&, t] {
                for (unsigned i = 0; i < 12; ++i)
                    expect("thread-" + std::to_string(t) + "-" + std::to_string(i), "thread-scope",
                           Json{{"thread", t}, {"i", i}});
            }));
        for (auto& thread : threads)
            thread.get();
        server.stop();
        std::cout << "Local HTTP reuse: fresh/reused identity, header/body reset, scope/port changes, close, "
                     "cancellation, timeout, oversize, redirect refusal and four-thread isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
