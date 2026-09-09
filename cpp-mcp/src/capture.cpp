#include "devbox/scoped_thread.hpp"
#include "devbox/capture.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace devbox {
namespace {
volatile std::sig_atomic_t capture_interrupted = 0;
void capture_signal(int) {
    capture_interrupted = 1;
}
void validate_quality(unsigned quality) {
    if (quality < 1 || quality > 100)
        throw Error("quality must be between 1 and 100.");
}
std::uint32_t parse_number(std::string_view text, const char* name) {
    std::uint32_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size())
        throw Error(std::string("parse capture ") + name);
    return number;
}
} // namespace
void validate_capture_image(std::span<const std::uint8_t> image, std::string_view mime) {
    if (mime == "image/jpeg") {
        if (image.size() < 5 || image[0] != 0xff || image[1] != 0xd8 || image[2] != 0xff ||
            image[image.size() - 2] != 0xff || image.back() != 0xd9)
            throw Error("Screen capture did not return a valid JPEG image.");
    } else if (mime == "image/png") {
        constexpr std::uint8_t signature[]{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        if (image.size() < sizeof(signature) ||
            !std::equal(std::begin(signature), std::end(signature), image.begin()))
            throw Error("Screen capture did not return a valid PNG image.");
    } else
        throw Error("Screen capture returned unsupported MIME type \"" + std::string(mime) + "\".");
}
bool transient_capture_error(const std::exception& error) {
    if (const auto* process = dynamic_cast<const ProcessError*>(&error); process && process->timed_out)
        return true;
    const auto message = lower(error.what());
    for (const auto* needle : {"timed out", "timeout", "printwindow", "bitblt", "getdc", "desktop compositor",
                               "capture did not return", "capture-worker metadata"})
        if (message.find(needle) != message.npos)
            return true;
    return false;
}
ImageCapture CaptureService::attempt(std::optional<std::uint32_t> pid, unsigned quality, bool tree,
                                     const Cancel& cancel) {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-capture-" + uuid());
#ifdef _WIN32
    if (!fs::create_directory(root))
        throw Error("create private capture temporary directory");
#else
    if (::mkdir(root.c_str(), 0700) != 0)
        throw Error("create private capture temporary directory");
#endif
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    const auto path = root / "capture.bin";
    std::vector<std::string> args{"--capture-worker", path_text(path), pid ? "program" : "display",
                                  std::to_string(quality)};
    if (pid) {
        args.push_back(std::to_string(*pid));
        args.push_back(tree ? "true" : "false");
    }
    ProcessOptions options;
    options.timeout = Millis(std::max<std::uint64_t>(1, config_->screen_capture_attempt_timeout_ms));
    options.max_capture_chars = 200000;
    auto result = spawn_process(path_text(executable_path()), args, options, cancel);
    Json metadata;
    try {
        metadata = Json::parse(trim(result.stdout_text));
    } catch (const std::exception&) {
        throw Error("parse C++ capture-worker metadata");
    }
    if (!metadata.is_object())
        throw Error("capture-worker metadata must be a JSON object");
    const auto mime = json_string(metadata, "mime_type");
    if (mime.empty())
        throw Error("capture-worker metadata omitted mime_type");
    const auto bytes = read_file(path, 64 * 1024 * 1024);
    std::vector<std::uint8_t> image(bytes.begin(), bytes.end());
    validate_capture_image(image, mime);
    metadata["bytes"] = image.size();
    metadata["sha256"] = sha256(image);
    return {std::move(image), mime, std::move(metadata)};
}
asio::awaitable<ImageCapture> CaptureService::capture(std::optional<std::uint32_t> pid, unsigned quality,
                                                      bool tree, Cancel cancel) {
    validate_quality(quality);
    if (pid && *pid == 0)
        throw Error("pid must be a positive process ID.");
    const auto start = Clock::now();
    const auto queue_timeout = Millis(std::max<std::uint64_t>(1, config_->screen_capture_queue_timeout_ms));
    for (;;) {
        if (cancel && cancel->cancelled())
            throw Error("Screen capture cancelled while waiting for the capture worker.");
        bool available = false;
        if (occupied_.compare_exchange_strong(available, true))
            break;
        if (Clock::now() - start >= queue_timeout)
            throw Error("Screen capture queue remained busy for " + std::to_string(queue_timeout.count()) +
                        " ms. Retry shortly.");
        co_await async_delay(Millis(10), cancel);
    }
    ScopeExit release([this] { occupied_.store(false); });
    const auto waited = std::chrono::duration_cast<Millis>(Clock::now() - start).count();
    const auto attempts = std::max<std::size_t>(1, config_->screen_capture_retries + 1);
    const auto timeout = std::max<std::uint64_t>(1, config_->screen_capture_attempt_timeout_ms);
    const auto overall = timeout * attempts + 150 * (attempts - 1);
    std::exception_ptr last;
    std::size_t completed = 0;
    for (std::size_t index = 1; index <= attempts; ++index) {
        completed = index;
        bool retry = false;
        try {
            auto pending = workers_.run(
                [this, pid, quality, tree, cancel] { return attempt(pid, quality, tree, cancel); }, cancel);
            auto value = co_await std::move(pending);
            value.metadata["capture_attempts"] = index;
            value.metadata["capture_retried"] = index > 1;
            value.metadata["capture_queue_wait_ms"] = waited;
            value.metadata["capture_attempt_timeout_ms"] = timeout;
            value.metadata["capture_overall_timeout_ms"] = overall;
            co_return value;
        } catch (const std::exception& e) {
            last = std::current_exception();
            retry = transient_capture_error(e);
        }
        if (!retry || index == attempts)
            break;
        if (cancel && cancel->cancelled())
            throw Error("Screen capture cancelled before retry.");
        co_await async_delay(Millis(150), cancel);
    }
    try {
        if (last)
            std::rethrow_exception(last);
    } catch (const ProcessError&) {
        throw;
    } catch (const std::exception&) {
    }
    throw Error("Screen capture failed after " + std::to_string(completed) + " attempt(s); queue_wait_ms=" +
                std::to_string(waited) + "; attempt_timeout_ms=" + std::to_string(timeout) +
                "; overall_timeout_ms=" + std::to_string(overall));
}
int run_capture_worker(const std::vector<std::string>& arguments) {
    try {
        if (arguments.size() != 3 && arguments.size() != 5)
            throw Error("capture worker requires output path, mode, quality, and optional pid/process-tree");
        const auto output = path_from_utf8(arguments[0]);
        const auto quality = parse_number(arguments[2], "quality");
        validate_quality(quality);
        std::optional<std::uint32_t> pid;
        bool tree = true;
        if (arguments[1] == "program" && arguments.size() == 5) {
            pid = parse_number(arguments[3], "pid");
            if (arguments[4] != "true" && arguments[4] != "false")
                throw Error("parse include-process-tree");
            tree = arguments[4] == "true";
        } else if (arguments[1] != "display" || arguments.size() != 3)
            throw Error("unsupported capture-worker mode");
        auto cancel = std::make_shared<Cancellation>();
#ifndef _WIN32
        capture_interrupted = 0;
        const auto old_int = std::signal(SIGINT, capture_signal);
        const auto old_term = std::signal(SIGTERM, capture_signal);
        ScopeExit restore([&] {
            std::signal(SIGINT, old_int);
            std::signal(SIGTERM, old_term);
        });
        ScopedThread signals([cancel](ThreadStopToken stop) {
            while (!stop.stop_requested()) {
                if (capture_interrupted) {
                    cancel->cancel();
                    return;
                }
                std::this_thread::sleep_for(Millis(10));
            }
        });
#endif
        auto value = native_capture(pid, quality, tree, cancel);
        cancel->check();
        validate_capture_image(value.image, value.mime_type);
        if (!output.parent_path().empty())
            fs::create_directories(output.parent_path());
        write_file(output,
                   std::string_view(reinterpret_cast<const char*>(value.image.data()), value.image.size()));
        value.metadata["mime_type"] = value.mime_type;
        std::cout << value.metadata.dump() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
} // namespace devbox
