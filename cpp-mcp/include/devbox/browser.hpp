#pragma once
#include "config.hpp"
namespace devbox {
std::string browser_page_status(const Json& observation, int http_status = 0);
std::string browser_normalize_url(std::string_view supplied);
bool browser_click_controls_match(const Json& previous, const Json& current, double x, double y);
// Internal launch policy, never an MCP argument: fresh owned debuggers receive
// bounded cold-start slack; shared endpoints retain the short stale-peer limit.
Clock::time_point browser_connection_deadline(bool shared, Clock::time_point operation_deadline,
                                              Clock::time_point now = Clock::now());
// Chromium creates this private file before publishing its complete endpoint,
// and its Windows writer can hold a sharing lock. Wait for these transient
// states within the existing deadline; permission and malformed values fail.
std::string read_browser_endpoint(const fs::path& profile, Clock::time_point deadline,
                                  const Cancel& cancel = {});
bool browser_process_arguments_match(std::string_view command, std::string_view executable,
                                     std::string_view profile, bool require_compatibility = false);
std::uint64_t browser_cgroup_headroom(std::uint64_t limit, std::uint64_t usage,
                                      std::uint64_t inactive_file_cache);
struct BrowserOptions {
    // Native qualification injection only. Neither is an MCP argument.
    std::optional<fs::path> executable;
    std::optional<std::uint16_t> fixture_port;
    // Native test/operator configuration only; never supplied by an MCP caller.
    std::optional<fs::path> shared_profile, shared_status_file;
    std::function<void(std::uint32_t)> pid_observer;
    // Native qualification diagnostics only; never exposed as an MCP argument.
    std::function<void(std::string_view)> launch_diagnostics;
    bool headed = false;
    bool text_only = false;
    bool compatibility = false;
    // Operator choice for virtual displays without a GPU. Retains Chromium's
    // sandbox and software rendering; ordinary host browsers keep their default.
    bool software_rendering = false;
    // Native regression injection only. Models and environment cannot set this.
    bool defer_initial_navigation = false;
    // Qualification injections only; never supplied through MCP or environment.
    bool defer_initial_observation = false;
    Millis navigation_wait{30000};
    std::function<void()> navigation_acknowledgement_probe;
    // Leave bounded time for a website agent to inspect an image and plan its next action.
    // Resource-pressure cleanup and the one-session limit remain independent safeguards.
    Millis lifetime{600000}, idle_timeout{180000};
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
