#pragma once
#include "capture.hpp"
#include <memory>

namespace devbox {
// Opt-in, local X11 session only. Wayland/Android do not expose native input here.
bool computer_x11_enabled();
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
