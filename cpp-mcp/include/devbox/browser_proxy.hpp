#pragma once
#include "common.hpp"
namespace devbox {
// Ephemeral loopback egress gate for an owned browser. All resolved addresses and
// the connected peer must pass the same public-address policy as web fetch.
class BrowserProxy {
    struct State;
    std::unique_ptr<State> state_;

  public:
    // Fixture injection is native-only; never populated from MCP or environment.
    explicit BrowserProxy(std::optional<std::uint16_t> fixture_port = {});
    ~BrowserProxy();
    std::uint16_t port() const;
    Json metrics() const;
    BrowserProxy(const BrowserProxy&) = delete;
    BrowserProxy& operator=(const BrowserProxy&) = delete;
};
} // namespace devbox
