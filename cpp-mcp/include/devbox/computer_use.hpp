#pragma once
#include "capture.hpp"
#include <memory>

namespace devbox {
class ComputerUse {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit ComputerUse(bool allow_broker = true);
    ~ComputerUse();
    ComputerUse(const ComputerUse&) = delete;
    ComputerUse& operator=(const ComputerUse&) = delete;
    Json windows(const Json& arguments, const Cancel& cancel);
    ImageCapture perform(const Json& arguments, const Cancel& cancel);
};
// Local-only IPC for an authenticated service running outside the interactive session.
Json computer_broker_call(std::string_view pipe, std::string_view operation, const Json& arguments,
                          const Cancel& cancel);
int run_computer_broker(std::string_view pipe, const std::function<bool()>& stopping);
// Coordinates are physical screen pixels; this only captures and never injects input.
ImageCapture native_capture_region(int left, int top, int width, int height, unsigned max_width,
                                   unsigned quality, const Cancel& cancel);
} // namespace devbox
