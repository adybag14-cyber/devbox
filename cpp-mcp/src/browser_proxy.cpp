#include "devbox/browser_proxy.hpp"
#include "devbox/research.hpp"
#include <array>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <map>
#include <thread>

namespace devbox {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = asio::ip::tcp;
constexpr std::uint64_t byte_limit = 64 * 1024 * 1024;
constexpr std::size_t connection_limit = 48;
} // namespace
struct BrowserProxy::State {
    // Counters outlive io's pending-handler destruction (Connection releases one).
    std::atomic_uint64_t bytes{0}, accepted{0}, denied{0}, active{0}, capacity_denied{0};
    asio::io_context io;
    Tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::optional<std::uint16_t> fixture;
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::thread worker;
    struct Connection : std::enable_shared_from_this<Connection> {
        State& owner;
        Tcp::socket client, remote;
        Tcp::resolver resolver;
        asio::steady_timer deadline;
        bool finished = false;
        explicit Connection(State& state, Tcp::socket socket)
            : owner(state), client(std::move(socket)), remote(state.io), resolver(state.io),
              deadline(state.io) {}
        ~Connection() {
            --owner.active;
        }
        void close() {
            if (finished)
                return;
            finished = true;
            boost::system::error_code ignored;
            resolver.cancel();
            deadline.cancel();
            client.cancel(ignored);
            client.close(ignored);
            remote.cancel(ignored);
            remote.close(ignored);
        }
        bool allowed(const Tcp::endpoint& endpoint) const {
            return web::public_address(endpoint.address().to_string()) ||
                   (owner.fixture && endpoint.address() == asio::ip::make_address("127.0.0.1") &&
                    endpoint.port() == *owner.fixture);
        }
        void count(std::size_t size) {
            if (owner.bytes.fetch_add(size) + size > byte_limit)
                throw Error("BROWSER_NETWORK_BYTE_BUDGET");
        }
        asio::awaitable<void> copy(Tcp::socket& from, Tcp::socket& to) {
            std::array<char, 16384> buffer;
            try {
                for (;;) {
                    const auto n = co_await from.async_read_some(asio::buffer(buffer), asio::use_awaitable);
                    count(n);
                    co_await asio::async_write(to, asio::buffer(buffer.data(), n), asio::use_awaitable);
                }
            } catch (...) {
                close();
            }
        }
        asio::awaitable<void> run() {
            auto self = shared_from_this();
            deadline.expires_after(std::chrono::seconds(45));
            deadline.async_wait([self](boost::system::error_code ec) {
                if (!ec)
                    self->close();
            });
            try {
                beast::flat_buffer buffer(16384 + 1024 * 1024);
                http::request_parser<http::string_body> parser;
                parser.header_limit(16384);
                parser.body_limit(1024 * 1024);
                co_await http::async_read(client, buffer, parser, asio::use_awaitable);
                auto request = parser.release();
                if (buffer.size())
                    throw Error("BROWSER_PROXY_PIPELINING_DENIED");
                const bool tunnel = request.method() == http::verb::connect;
                const auto target = std::string(request.target());
                auto supplied = tunnel ? "https://" + target + "/" : target;
                Url url;
                bool native_fixture = false;
                if (owner.fixture) {
                    url = Url::parse(supplied);
                    native_fixture = url.host == "127.0.0.1" && url.port == std::to_string(*owner.fixture) &&
                                     url.userinfo.empty() && url.scheme == (tunnel ? "https" : "http");
                }
                if (!native_fixture)
                    url = Url::parse(web::normalize_url(supplied));
                if ((!tunnel && url.scheme != "http") ||
                    (tunnel &&
                     (url.path != "/" || url.has_query || url.has_fragment || !url.userinfo.empty())))
                    throw Error("BROWSER_PROXY_TARGET_DENIED");
                const auto remote_port = url.port.empty() ? (tunnel ? "443" : "80") : url.port;
                auto endpoints = co_await resolver.async_resolve(url.host, remote_port, asio::use_awaitable);
                if (endpoints.empty())
                    throw Error("BROWSER_PROXY_DNS_EMPTY");
                for (const auto& entry : endpoints)
                    if (!allowed(entry.endpoint()))
                        throw Error("BROWSER_PROXY_PRIVATE_ADDRESS");
                co_await asio::async_connect(remote, endpoints, asio::use_awaitable);
                if (!allowed(remote.remote_endpoint()))
                    throw Error("BROWSER_PROXY_PRIVATE_PEER");
                ++owner.accepted;
                if (tunnel) {
                    if (!request.body().empty())
                        throw Error("BROWSER_PROXY_CONNECT_BODY");
                    const std::string ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
                    co_await asio::async_write(client, asio::buffer(ok), asio::use_awaitable);
                } else {
                    request.target(url.path + (url.has_query ? "?" + url.query : ""));
                    request.set(http::field::host, url.host + (url.port.empty() ? "" : ":" + url.port));
                    request.erase(http::field::proxy_authorization);
                    request.erase(http::field::proxy_connection);
                    request.keep_alive(false);
                    count(request.body().size());
                    co_await http::async_write(remote, request, asio::use_awaitable);
                }
                asio::co_spawn(
                    owner.io,
                    [self]() -> asio::awaitable<void> { co_await self->copy(self->remote, self->client); },
                    asio::detached);
                if (tunnel)
                    co_await copy(client, remote);
                // For plain HTTP, the response pump owns the connection until EOF.
            } catch (...) {
                ++owner.denied;
                close();
            }
        }
    };
    explicit State(std::optional<std::uint16_t> fixture_port) : fixture(fixture_port) {
        asio::co_spawn(
            io,
            [this]() -> asio::awaitable<void> {
                try {
                    for (;;) {
                        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                        if (active >= connection_limit || bytes >= byte_limit) {
                            ++denied;
                            ++capacity_denied;
                            continue;
                        }
                        ++active;
                        auto connection = std::make_shared<Connection>(*this, std::move(socket));
                        asio::co_spawn(
                            io, [connection]() -> asio::awaitable<void> { co_await connection->run(); },
                            asio::detached);
                    }
                } catch (...) {
                }
            },
            asio::detached);
        worker = std::thread([this] { io.run(); });
    }
    ~State() {
        io.stop();
        if (worker.joinable())
            worker.join();
    }
};
BrowserProxy::BrowserProxy(std::optional<std::uint16_t> fixture) : state_(std::make_unique<State>(fixture)) {}
BrowserProxy::~BrowserProxy() = default;
std::uint16_t BrowserProxy::port() const {
    return state_->port;
}
Json BrowserProxy::metrics() const {
    return Json{{"transferred_bytes", state_->bytes.load()},
                {"byte_limit", byte_limit},
                {"accepted_connections", state_->accepted.load()},
                {"denied_connections", state_->denied.load()},
                {"capacity_denials", state_->capacity_denied.load()},
                {"active_connections", state_->active.load()},
                {"connection_limit", connection_limit},
                {"egress", "public_http_https_only"}};
}
} // namespace devbox
