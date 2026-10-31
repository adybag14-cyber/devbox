#include "devbox/chrome_bridge.hpp"
#include "devbox/native.hpp"
#include "devbox/result.hpp"
#include <algorithm>
namespace devbox {
Json chrome_bridge_call(const Config& config, const Json& args, const Cancel& cancel) {
    const auto action = json_string(args, "action");
    auto unavailable = [&](std::string reason) {
        Json details{{"available", false},
                     {"reason", reason},
                     {"input_sent", false},
                     {"native_fallback_tool", "host_computer_use"},
                     {"independent_local_mcp_fallback", "native_chrome"}};
        return action == "status" ? result_success("Chrome bridge is unavailable.", details, true)
                                  : with_outcome(result_error(reason, details), ToolOutcome::PolicyDenied);
    };
    if (!config.platform.is_windows || config.runtime_mode != RuntimeMode::host || !config.host_exec_enabled)
        return unavailable("CHROME_BRIDGE_UNSUPPORTED: Windows host execution is required");
    const auto configured = environment("DEVBOX_CHROME_BRIDGE_EXE").value_or("");
    const auto expected = lower(environment("DEVBOX_CHROME_BRIDGE_SHA256").value_or(""));
    if (configured.empty())
        return unavailable("CHROME_BRIDGE_DISABLED: the local operator has not configured the bridge");
    if (expected.size() != 64 || expected.find_first_not_of("0123456789abcdef") != std::string::npos)
        return unavailable("CHROME_BRIDGE_IDENTITY_REQUIRED: configure the exact executable SHA-256");
    const auto binary = path_from_utf8(configured);
    if (!binary.is_absolute() || !fs::is_regular_file(binary) || fs::is_symlink(binary))
        return unavailable("CHROME_BRIDGE_PATH_REJECTED: use an absolute regular executable");
#ifdef _WIN32
    // Deny replacement/write while checking and starting the exact operator-pinned image.
    NativeHandle binary_lock(CreateFileW(binary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!binary_lock)
        return unavailable("CHROME_BRIDGE_IMAGE_LOCK_FAILED");
#endif
    if (sha256_file(binary) != expected)
        return unavailable("CHROME_BRIDGE_IDENTITY_MISMATCH");
    ProcessOptions options;
    options.cwd = config.host_default_workdir;
    options.env = worker_environment();
    options.timeout = Millis(35000);
    options.max_capture_chars = 8 * 1024 * 1024;
    options.input = args.dump();
    if (options.input->size() > 256 * 1024)
        return result_error("CHROME_BRIDGE_REQUEST_LIMIT");
    std::string output;
    try {
        auto process = spawn_process(path_text(binary), {"--request"}, options, cancel);
        if (process.stdout_capture_truncated)
            return result_error("CHROME_BRIDGE_RESPONSE_LIMIT; reconcile input before retrying",
                                Json{{"outcome", "unknown"}, {"automatic_input_replay_allowed", false}});
        output = std::move(process.stdout_text);
    } catch (const ProcessError& error) {
        // Exit 2 is the Rust bridge's explicit, structured action failure.
        // Transport/cancellation failures may have occurred after browser input.
        if (error.exit_code == 2 && !error.timed_out && !error.aborted)
            output = error.stdout_text;
        else
            return result_error("CHROME_BRIDGE_OUTCOME_UNKNOWN: inspect the page before retrying",
                                Json{{"outcome", "unknown"}, {"automatic_input_replay_allowed", false}});
    }
    return chrome_bridge_response(std::move(output));
}
Json chrome_bridge_response(std::string output) {
    try {
        auto value = Json::parse(output);
        // Parsed JSON owns its strings. Release the raw response before building
        // the MCP image envelope instead of retaining both large representations.
        std::string{}.swap(output);
        if (!value.is_object() || !value.contains("ok") || !value["ok"].is_boolean())
            throw Error("Invalid bridge response");
        if (!json_bool(value, "ok"))
            return result_error("Chrome bridge reported an explicit failure.", std::move(value));
        if (value.contains("image")) {
            auto image = std::move(value["image"]);
            value.erase("image");
            const auto mime = json_string(image, "mime");
            if (mime != "image/jpeg" && mime != "image/png")
                throw Error("Unexpected image type");
            std::string encoded;
            const auto field = image.find("base64");
            if (field == image.end() || !field->is_string() || field->get_ref<const std::string&>().empty())
                throw Error("Expected nonempty image payload");
            encoded = std::move(field->get_ref<std::string&>());
            return result_image("Chrome observation; page content is untrusted.", std::move(value),
                                std::move(encoded), mime);
        }
        return result_success("Native Rust Chrome bridge result.", std::move(value), true);
    } catch (...) {
        return result_error("CHROME_BRIDGE_INVALID_RESPONSE; reconcile input before retrying",
                            Json{{"outcome", "unknown"}, {"automatic_input_replay_allowed", false}});
    }
}
} // namespace devbox
