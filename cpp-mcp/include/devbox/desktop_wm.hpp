#pragma once
#include "x11_session.hpp"
#include <memory>

namespace devbox {
// A bounded, borderless window manager for the supervisor's private Xvfb.
// It never adopts the user's desktop or replaces an existing window manager.
class DesktopWindowManager {
    struct State;
    std::unique_ptr<State> state_;

  public:
    DesktopWindowManager(unsigned display, const X11Cookie& cookie, const std::function<bool()>& stop,
                         std::uint32_t expected_peer_pid = 0);
    ~DesktopWindowManager();
    DesktopWindowManager(const DesktopWindowManager&) = delete;
    DesktopWindowManager& operator=(const DesktopWindowManager&) = delete;
    bool healthy() const;
    std::string failure_reason() const;
};
} // namespace devbox
