#include "devbox/result.hpp"
#include "devbox/server.hpp"
#include "devbox/state_store.hpp"
#include "devbox/state_transport.hpp"
#include <boost/beast.hpp>
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F fn, std::string_view prefix) {
    try {
        fn();
    } catch (const std::exception& e) {
        if (std::string_view(e.what()).find(prefix) != std::string_view::npos)
            return;
        throw;
    }
    throw Error("Expected rejection: " + std::string(prefix));
}
struct Echo final : McpBackend {
    std::atomic_uint calls{0};
    Json server_info() const override {
        return Json{{"name", "owned-state-transport-test"}, {"version", "1"}};
    }
    Json list_tools(std::string_view) const override {
        return Json::array({Json{{"name", "echo"}}});
    }
    bool has_tool(std::string_view name) const override {
        return name == "echo";
    }
    bool ready() const override {
        return true;
    }
    asio::awaitable<Json> metadata(const HttpRequest&) override {
        co_return server_info();
    }
    void observe_http(const HttpRequest&, int, std::uint64_t, Millis, bool) override {}
    asio::awaitable<Json> call_tool(std::string, Json args, Cancel cancel) override {
        ++calls;
        if (json_uint(args, "delay"))
            co_await async_delay(Millis(json_uint(args, "delay")), cancel);
        co_return result_explicit("echo", std::move(args), "echo");
    }
};
std::string request(Json args) {
    return Json{{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "tools/call"},
                {"params", {{"name", "echo"}, {"arguments", std::move(args)}}}}
        .dump();
}
void verify(StateHttpTransport::Transfer& transfer, const Json& expected) {
    require(transfer.response().status == 200, "HTTP success");
    require(Json::parse(transfer.response().body)["result"]["structuredContent"]["data"] == expected,
            "No request/response data bleed");
    // This direct transport test has its own response oracle; production instead
    // performs authenticated state-protocol verification before this call.
    transfer.verified();
}
// One bounded, nonblocking HTTP responder for redirect/header failure controls.
class RawReply {
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_{io_, {asio::ip::make_address("127.0.0.1"), 0}};
    std::jthread worker_;

  public:
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    explicit RawReply(std::string reply) {
        acceptor_.non_blocking(true);
        worker_ = std::jthread([this, reply = std::move(reply)](std::stop_token stop) {
            asio::ip::tcp::socket socket(io_);
            const auto deadline = Clock::now() + Millis(3000);
            boost::system::error_code ec;
            while (!stop.stop_requested() && Clock::now() < deadline) {
                acceptor_.accept(socket, ec);
                if (!ec)
                    break;
                if (ec != asio::error::would_block && ec != asio::error::try_again)
                    return;
                std::this_thread::sleep_for(Millis(1));
            }
            if (ec || !socket.is_open())
                return;
            socket.non_blocking(true, ec);
            std::array<char, 4096> bytes{};
            while (!stop.stop_requested() && Clock::now() < deadline) {
                if (socket.read_some(asio::buffer(bytes), ec) > 0)
                    break;
                if (ec != asio::error::would_block && ec != asio::error::try_again)
                    return;
                std::this_thread::sleep_for(Millis(1));
            }
            std::size_t sent = 0;
            while (sent < reply.size() && !stop.stop_requested() && Clock::now() < deadline) {
                sent += socket.write_some(asio::buffer(reply.data() + sent, reply.size() - sent), ec);
                if (ec && ec != asio::error::would_block && ec != asio::error::try_again)
                    break;
                if (ec)
                    std::this_thread::sleep_for(Millis(1));
            }
            socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket.close(ec);
        });
    }
};
int run() {
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-state-transport-" + uuid());
    ensure_private_state_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    auto config = std::make_shared<Config>();
    config->host = "127.0.0.1";
    config->port = 0;
    config->project_root = root;
    auto echo = std::make_shared<Echo>();
    HttpServer server(config, echo);
    const auto port = server.start();
    const StateHttpTransport::Peer peer{port, 1, 1, 1};
    Json report = Json::array();
    for (bool reuse : {false, true}) {
        StateHttpTransport transport(reuse);
        for (unsigned i = 0; i < 12; ++i) {
            const Json args{{"number", i}, {"text", ((i % 2) != 0) ? "first" : "second"}};
            auto transfer = transport.post(peer, request(args), Millis(2000), 65536);
            verify(transfer, args);
        }
        const auto stats = transport.statistics();
        require(stats.transfers == 12 && stats.connections == (reuse ? 1U : 12U),
                "Actual TCP connection reuse differs from policy");
        require(stats.idle == (reuse ? 1U : 0U), "Idle connection count differs from policy");
        report.push_back(
            Json{{"reuse", reuse}, {"transfers", stats.transfers}, {"connections", stats.connections}});
    }
    StateHttpTransport transport;
    const Json small{{"payload", "fixed"}};
    {
        auto unverified = transport.post(peer, request(small), Millis(2000), 65536);
    }
    require(transport.statistics().idle == 0, "Unverified reply never re-enters pool");
    {
        auto ok = transport.post(peer, request(small), Millis(2000), 65536);
        verify(ok, small);
    }
    auto changed = peer;
    ++changed.generation;
    const auto before = transport.statistics().connections;
    {
        auto ok = transport.post(changed, request(small), Millis(2000), 65536);
        verify(ok, small);
    }
    require(transport.statistics().connections == before + 1, "Generation switch closes the old channel");
    {
        auto old = transport.post(changed, request(small), Millis(2000), 65536);
        verify(old, small);
        transport.discard_idle();
    }
    require(transport.statistics().idle == 0, "In-flight lease cannot repopulate an invalidated epoch");
    {
        std::vector<StateHttpTransport::Transfer> held;
        for (unsigned i = 0; i < 8; ++i) {
            held.push_back(transport.post(peer, request(small), Millis(2000), 65536));
            verify(held.back(), small);
        }
        require(transport.statistics().idle == 0, "Borrowed handles are exclusively owned");
        held.clear();
    }
    require(transport.statistics().idle == 4, "Pool retains at most four channels");
    {
        std::vector<std::future<void>> concurrent;
        for (unsigned t = 0; t < 8; ++t)
            concurrent.push_back(std::async(std::launch::async, [&, t] {
                for (unsigned i = 0; i < 20; ++i) {
                    const Json args{{"thread", t}, {"call", i}};
                    auto reply = transport.post(peer, request(args), Millis(3000), 65536);
                    verify(reply, args);
                }
            }));
        for (auto& done : concurrent)
            done.get();
    }
    require(transport.statistics().idle <= 4, "Concurrent fallback does not expand idle capacity");
    transport.discard_idle();
    rejects(
        [&] {
            (void)transport.post(peer, request(Json{{"payload", std::string(1000, 'x')}}), Millis(2000), 128);
        },
        "STATE_IPC_FRAME_LIMIT");
    require(transport.statistics().idle == 0, "Oversized response is discarded");
    {
        auto ok = transport.post(peer, request(small), Millis(2000), 65536);
        verify(ok, small);
    }
    transport.discard_idle();
    auto cancel = std::make_shared<Cancellation>();
    cancel->cancel();
    const auto sent = echo->calls.load();
    rejects([&] { (void)transport.post(peer, request(small), Millis(2000), 65536, cancel); }, "cancel");
    require(echo->calls == sent, "Pre-cancelled request never dispatched");
    rejects([&] { (void)transport.post(peer, request(Json{{"delay", 200}}), Millis(10), 65536); },
            "HTTP request failed");
    require(transport.statistics().idle == 0, "Timeout destroys the channel");
    {
        auto pending_cancel = std::make_shared<Cancellation>();
        std::jthread request_cancel([pending_cancel] {
            std::this_thread::sleep_for(Millis(20));
            pending_cancel->cancel();
        });
        bool cancelled = false;
        try {
            (void)transport.post(peer, request(Json{{"delay", 500}}), Millis(200), 65536, pending_cancel);
        } catch (const Cancelled&) {
            cancelled = true;
        }
        require(cancelled, "In-flight cancellation is preserved");
    }
    {
        auto ok = transport.post(peer, request(small), Millis(2000), 65536);
        verify(ok, small);
    }
    transport.discard_idle();
    {
        RawReply redirect("HTTP/1.1 307 Temporary Redirect\r\nLocation: http://127.0.0.1:" +
                          std::to_string(port) + "/mcp\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        auto other = peer;
        other.port = redirect.port();
        const auto count = echo->calls.load();
        auto reply = transport.post(other, request(small), Millis(2000), 65536);
        require(reply.response().status == 307 && echo->calls == count,
                "Redirect must not dispatch to another endpoint");
        reply.verified(); // Even an incorrect caller approval cannot pool non-200.
    }
    require(transport.statistics().idle == 0, "Redirect reply is not retained");
    {
        RawReply huge("HTTP/1.1 200 OK\r\nX-Large: " + std::string(33000, 'x') +
                      "\r\nContent-Length: 0\r\n\r\n");
        auto other = peer;
        other.port = huge.port();
        rejects([&] { (void)transport.post(other, request(small), Millis(2000), 65536); },
                "STATE_IPC_FRAME_LIMIT");
    }
    {
        auto ok = transport.post(peer, request(small), Millis(2000), 65536);
        verify(ok, small);
    }
    auto invalid = peer;
    invalid.port = 0;
    rejects([&] { (void)transport.post(invalid, request(small), Millis(100), 65536); },
            "STATE_IPC_INVALID_PEER");
    rejects([&] { (void)transport.post(peer, request(small), Millis(100), 0); }, "STATE_IPC_FRAME_LIMIT");
    server.stop();
    std::cout << Json{{"ok", true},    {"connection_comparison", report}, {"concurrent_requests", 160},
                      {"max_idle", 4}, {"redirect_followed", false},      {"production_requests", 0}}
                     .dump(2)
              << '\n';
    return 0;
}
} // namespace
int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
