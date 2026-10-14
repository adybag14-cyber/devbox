#pragma once
#include "config.hpp"
namespace devbox {
std::string browser_page_status(const Json& observation, int http_status = 0);
std::string browser_normalize_url(std::string_view supplied);
struct BrowserOptions {
    // Native qualification injection only. Neither is an MCP argument.
    std::optional<fs::path> executable;
    std::optional<std::uint16_t> fixture_port;
    // Native test/operator configuration only; never supplied by an MCP caller.
    std::optional<fs::path> shared_profile, shared_status_file;
    std::function<void(std::uint32_t)> pid_observer;
    bool headed = false;
    Millis lifetime{300000}, idle_timeout{60000};
};
struct BrowserObservation {
    Json data;
    std::string image_base64;
};
class BrowserService {
    struct State;
    std::unique_ptr<State> state_;

  public:
    explicit BrowserService(std::shared_ptr<const Config> config, BrowserOptions options = {});
    ~BrowserService();
    BrowserObservation perform(const Json& args, std::string_view principal, const Cancel& cancel = {});
    void stop();
};
} // namespace devbox
