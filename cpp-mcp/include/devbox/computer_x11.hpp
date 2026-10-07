#pragma once
#include "capture.hpp"
#include <memory>
#include <set>

namespace devbox {
// Opt-in, local X11 session only. Wayland/Android do not expose native input here.
bool computer_x11_enabled();
// Read-only local X11 capture. An empty PID set captures the display. This path
// does not load XTEST, change focus, inject input, or require input opt-in.
ImageCapture capture_x11_native(const std::set<std::uint32_t>& pids, const Cancel& cancel);
class ComputerX11 {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ComputerX11();
    ~ComputerX11();
    Json windows(const Json&, const Cancel&);
    ImageCapture perform(const Json&, const Cancel&);
};
} // namespace devbox
