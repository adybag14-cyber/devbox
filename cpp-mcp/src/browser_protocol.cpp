#include "devbox/browser_protocol.hpp"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <map>
#include <set>

namespace devbox {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using Tcp = asio::ip::tcp;
constexpr std::size_t maximum_pending = 64, maximum_events = 2048, maximum_depth = 16;
bool browser_path_valid(std::string_view value) {
    constexpr std::string_view prefix = "/devtools/browser/";
    if (!value.starts_with(prefix) || value.size() <= prefix.size() || value.size() > 256)
        return false;
    for (const auto c : value.substr(prefix.size()))
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}
} // namespace

struct BrowserProtocol::State {
    asio::io_context io;
    websocket::stream<Tcp::socket> socket{io};
    std::size_t message_limit, depth = 0;
    std::uint64_t sequence = 0;
    bool connected = false;
    Event event;
    std::map<std::uint64_t, Json> replies;
    std::set<std::uint64_t> active;
    std::size_t pending_bytes = 0;
    std::uint64_t commands = 0, batches = 0, received_messages = 0, operation_micros = 0;
    explicit State(std::size_t limit) : message_limit(limit) {
        if (!limit || limit > 16 * 1024 * 1024)
            throw Error("BROWSER_PROTOCOL_LIMIT: invalid message bound");
        socket.read_message_max(limit);
    }
    void close() noexcept {
        connected = false;
        boost::system::error_code ignored;
        socket.next_layer().cancel(ignored);
        socket.next_layer().shutdown(Tcp::socket::shutdown_both, ignored);
        socket.next_layer().close(ignored);
        replies.clear();
        pending_bytes = 0;
    }
    template <class Start> void operation(Start start, Clock::time_point deadline, const Cancel& cancel) {
        const auto began = Clock::now();
        struct Timing {
            std::uint64_t& total;
            Clock::time_point began;
            ~Timing() {
                total += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count());
            }
        } timing{operation_micros, began};
        if (cancel)
            cancel->check();
        if (Clock::now() >= deadline)
            throw Error("BROWSER_PROTOCOL_DEADLINE");
        bool completed = false, expired = false, aborted = false;
        boost::system::error_code result;
        asio::steady_timer timer(io);
        std::function<void()> tick;
        tick = [&] {
            timer.expires_at(std::min(deadline, Clock::now() + Millis(25)));
            timer.async_wait([&](boost::system::error_code error) {
                if (error || completed)
                    return;
                aborted = cancel && cancel->cancelled();
                expired = Clock::now() >= deadline;
                if (aborted || expired)
                    close();
                else
                    tick();
            });
        };
        io.restart();
        start([&](boost::system::error_code error) {
            result = error;
            completed = true;
            timer.cancel();
        });
        tick();
        io.run();
        if (aborted || (cancel && cancel->cancelled()))
            throw Cancelled();
        if (expired || Clock::now() >= deadline) {
            close();
            throw Error("BROWSER_PROTOCOL_DEADLINE");
        }
        if (!completed || result) {
            close();
            throw Error("BROWSER_PROTOCOL_IO: " + (result ? result.message() : "incomplete operation"));
        }
    }
    Json receive(Clock::time_point deadline, const Cancel& cancel) {
        beast::flat_buffer buffer;
        operation(
            [&](auto done) {
                socket.async_read(buffer,
                                  [done](boost::system::error_code error, std::size_t) { done(error); });
            },
            deadline, cancel);
        if (!socket.got_text() || buffer.size() > message_limit)
            throw Error("BROWSER_PROTOCOL_MESSAGE: expected bounded JSON text");
        ++received_messages;
        std::size_t nodes = 0;
        return Json::parse(beast::buffers_to_string(buffer.data()),
                           [&](int depth, Json::parse_event_t, Json&) {
                               if (depth > 128 || ++nodes > 200000)
                                   throw Error("BROWSER_PROTOCOL_MESSAGE: JSON complexity exceeded");
                               return true;
                           });
    }
};

BrowserProtocol::BrowserProtocol(std::size_t limit) : state_(std::make_unique<State>(limit)) {}
BrowserProtocol::~BrowserProtocol() {
    close();
}
void BrowserProtocol::close() noexcept {
    state_->close();
}
void BrowserProtocol::on_event(Event handler) {
    state_->event = std::move(handler);
}
void BrowserProtocol::connect(std::uint16_t port, std::string_view path, Clock::time_point deadline,
                              const Cancel& cancel) {
    if (!port || !browser_path_valid(path) || state_->connected)
        throw Error("BROWSER_PROTOCOL_ENDPOINT: expected a new owned loopback browser endpoint");
    const Tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    state_->operation([&](auto done) { state_->socket.next_layer().async_connect(endpoint, done); }, deadline,
                      cancel);
    state_->socket.next_layer().set_option(Tcp::no_delay(true));
    const auto host = "127.0.0.1:" + std::to_string(port);
    const std::string target(path);
    state_->operation([&](auto done) { state_->socket.async_handshake(host, target, done); }, deadline,
                      cancel);
    state_->connected = true;
}
Json BrowserProtocol::call(std::string_view method, Json params, Clock::time_point deadline,
                           const Cancel& cancel, std::string_view session) {
    auto response = batch({{std::string(method), std::move(params)}}, deadline, cancel, session).front();
    if (response.contains("error"))
        throw Error("BROWSER_PROTOCOL_REMOTE: " +
                    json_string(response["error"], "message", "command failed").substr(0, 256));
    return std::move(response["result"]);
}
Json BrowserProtocol::metrics() const {
    return {{"commands", state_->commands},
            {"batches", state_->batches},
            {"received_messages", state_->received_messages},
            {"transport_microseconds", state_->operation_micros}};
}
std::vector<Json> BrowserProtocol::batch(const std::vector<std::pair<std::string, Json>>& commands,
                                         Clock::time_point deadline, const Cancel& cancel,
                                         std::string_view session) {
    auto& state = *state_;
    if (!state.connected || commands.empty() || commands.size() > 32 ||
        state.active.size() + commands.size() > maximum_pending || state.depth >= maximum_depth ||
        state.sequence > max_safe_integer - commands.size())
        throw Error("BROWSER_PROTOCOL_CALL: invalid state or request");
    std::vector<std::string> messages;
    std::size_t request_bytes = 0;
    const auto first_id = state.sequence + 1;
    for (const auto& [method, params] : commands) {
        if (method.empty() || method.size() > 128 || !params.is_object())
            throw Error("BROWSER_PROTOCOL_CALL: invalid state or request");
        Json request{{"id", first_id + messages.size()}, {"method", method}, {"params", params}};
        if (!session.empty())
            request["sessionId"] = session;
        auto bytes = request.dump();
        if (bytes.size() > state.message_limit - request_bytes)
            throw Error("BROWSER_PROTOCOL_LIMIT: request batch is too large");
        request_bytes += bytes.size();
        messages.push_back(std::move(bytes));
    }
    ++state.depth;
    struct Depth {
        std::size_t& value;
        ~Depth() {
            --value;
        }
    } depth{state.depth};
    state.sequence += messages.size();
    struct Active {
        State& state;
        std::uint64_t first;
        std::size_t count;
        int exceptions = std::uncaught_exceptions();
        bool received = false;
        ~Active() {
            for (std::size_t i = 0; i < count; ++i)
                state.active.erase(first + i);
            if (!received && std::uncaught_exceptions() > exceptions)
                state.close();
        }
    } active{state, first_id, messages.size()};
    for (std::size_t i = 0; i < messages.size(); ++i)
        state.active.insert(first_id + i);
    state.socket.text(true);
    for (const auto& bytes : messages)
        state.operation(
            [&](auto done) {
                state.socket.async_write(asio::buffer(bytes), [done](boost::system::error_code error,
                                                                     std::size_t) { done(error); });
            },
            deadline, cancel);
    state.commands += messages.size();
    if (messages.size() > 1)
        ++state.batches;
    std::size_t events = 0;
    std::size_t response_total = 0;
    std::vector<Json> results;
    for (;;) {
        const auto id = first_id + results.size();
        auto found = state.replies.find(id);
        if (found != state.replies.end()) {
            const auto size = found->second.dump().size();
            state.pending_bytes -= size;
            if (size > state.message_limit - response_total)
                throw Error("BROWSER_PROTOCOL_MESSAGE: batch result budget exceeded");
            response_total += size;
            auto response = std::move(found->second);
            state.replies.erase(found);
            const bool error =
                response.contains("error") && response["error"].is_object() && !response.contains("result");
            if (!error && (response.contains("error") || !response.contains("result") ||
                           !response["result"].is_object()))
                throw Error("BROWSER_PROTOCOL_MESSAGE: missing command result");
            results.push_back(std::move(response));
            // A duplicate reply for an already consumed command is invalid.
            state.active.erase(id);
            if (results.size() == messages.size()) {
                active.received = true;
                return results;
            }
            continue;
        }
        auto response = state.receive(deadline, cancel);
        if (!response.is_object())
            throw Error("BROWSER_PROTOCOL_MESSAGE: expected object");
        if (response.contains("id")) {
            if (!response["id"].is_number_unsigned() && !response["id"].is_number_integer())
                throw Error("BROWSER_PROTOCOL_MESSAGE: invalid response identity");
            const auto response_id = json_uint(response, "id");
            const auto response_bytes = response.dump().size();
            if (!state.active.contains(response_id) || state.replies.size() >= maximum_pending ||
                response_bytes > state.message_limit - state.pending_bytes ||
                !state.replies.emplace(response_id, std::move(response)).second)
                throw Error("BROWSER_PROTOCOL_MESSAGE: unexpected or repeated response identity");
            state.pending_bytes += response_bytes;
        } else {
            if (++events > maximum_events || !response.contains("method") || !response["method"].is_string())
                throw Error("BROWSER_PROTOCOL_MESSAGE: event budget or shape invalid");
            if (state.event)
                state.event(response, deadline, cancel);
        }
    }
}
} // namespace devbox
