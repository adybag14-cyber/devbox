#pragma once
#include "async.hpp"
#include "config.hpp"
#include "process.hpp"
#include <map>
namespace devbox {
struct ImageCapture {
    std::vector<std::uint8_t> image;
    std::string mime_type;
    Json metadata;
};
// Birth values must be ordered creation timestamps from the same clock (Windows
// FILETIME here), not opaque process-instance hashes from other platforms.
std::map<std::uint32_t, std::uint64_t>
capture_process_lineage(const std::map<std::uint32_t, std::uint32_t>& parents, std::uint32_t root,
                        const std::function<std::optional<std::uint64_t>(std::uint32_t)>& birth);
void validate_capture_image(std::span<const std::uint8_t> image, std::string_view mime);
bool transient_capture_error(const std::exception& error);
ImageCapture native_capture(std::optional<std::uint32_t> pid, unsigned quality, bool include_tree,
                            const Cancel& cancel = {});
int run_capture_worker(const std::vector<std::string>& arguments);
class CaptureService {
    std::shared_ptr<const Config> config_;
    std::atomic_bool occupied_{false};
    WorkPool workers_{1, 4};
    std::function<void(ProcessOptions&)> configure_worker_;
    ImageCapture attempt(std::optional<std::uint32_t> pid, unsigned quality, bool tree, const Cancel& cancel);

  public:
    // Native dependency injection for controlled worker fixtures; never exposed through MCP/env.
    explicit CaptureService(std::shared_ptr<const Config> config,
                            std::function<void(ProcessOptions&)> configure_worker = {})
        : config_(std::move(config)), configure_worker_(std::move(configure_worker)) {}
    asio::awaitable<ImageCapture> capture(std::optional<std::uint32_t> pid, unsigned quality, bool tree,
                                          Cancel cancel);
};
} // namespace devbox
