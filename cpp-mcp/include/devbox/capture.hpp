#pragma once
#include "async.hpp"
#include "config.hpp"
#include "process.hpp"
namespace devbox {
struct ImageCapture {
    std::vector<std::uint8_t> image;
    std::string mime_type;
    Json metadata;
};
void validate_capture_image(std::span<const std::uint8_t> image, std::string_view mime);
bool transient_capture_error(const std::exception& error);
ImageCapture native_capture(std::optional<std::uint32_t> pid, unsigned quality, bool include_tree,
                            const Cancel& cancel = {});
int run_capture_worker(const std::vector<std::string>& arguments);
class CaptureService {
    std::shared_ptr<const Config> config_;
    std::atomic_bool occupied_{false};
    WorkPool workers_{1, 4};
    ImageCapture attempt(std::optional<std::uint32_t> pid, unsigned quality, bool tree, const Cancel& cancel);

  public:
    explicit CaptureService(std::shared_ptr<const Config> config) : config_(std::move(config)) {}
    asio::awaitable<ImageCapture> capture(std::optional<std::uint32_t> pid, unsigned quality, bool tree,
                                          Cancel cancel);
};
} // namespace devbox
