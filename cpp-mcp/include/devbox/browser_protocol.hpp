#pragma once
#include "common.hpp"

namespace devbox {
// Private connection to a browser launched and owned by BrowserService. No MCP
// argument supplies a host, WebSocket URL, protocol method or JavaScript body.
class BrowserProtocol {
    struct State;
    std::unique_ptr<State> state_;

  public:
    using Event = std::function<void(const Json&, Clock::time_point, const Cancel&)>;
    explicit BrowserProtocol(std::size_t message_limit = 8 * 1024 * 1024);
    ~BrowserProtocol();
    BrowserProtocol(const BrowserProtocol&) = delete;
    BrowserProtocol& operator=(const BrowserProtocol&) = delete;
    void connect(std::uint16_t port, std::string_view browser_path, Clock::time_point deadline,
                 const Cancel& cancel = {});
    void on_event(Event handler);
    Json call(std::string_view method, Json params, Clock::time_point deadline, const Cancel& cancel = {},
              std::string_view session = {});
    void close() noexcept;
};
} // namespace devbox
