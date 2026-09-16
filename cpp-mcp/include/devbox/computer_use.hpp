#pragma once
#include "capture.hpp"
#include <memory>

namespace devbox {
class ComputerUse {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ComputerUse();
    ~ComputerUse();
    ComputerUse(const ComputerUse&) = delete;
    ComputerUse& operator=(const ComputerUse&) = delete;
    Json windows(const Json& arguments, const Cancel& cancel);
    ImageCapture perform(const Json& arguments, const Cancel& cancel);
};
// Coordinates are physical screen pixels; this only captures and never injects input.
ImageCapture native_capture_region(int left, int top, int width, int height, unsigned max_width,
                                   unsigned quality, const Cancel& cancel);
} // namespace devbox
