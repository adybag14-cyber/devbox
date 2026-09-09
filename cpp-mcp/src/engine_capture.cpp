#include "devbox/engine.hpp"
#include "devbox/result.hpp"
namespace devbox {
asio::awaitable<Json> Engine::capture(std::string name, Json args, Cancel cancel) {
    const auto display = name.find("display") != name.npos;
    const auto pid = json_uint(args, "pid");
    const auto title =
        config_->platform.is_windows ? "Windows Host" : config_->platform.display_name + " Host";
    if (!display && !pid)
        co_return result_error("pid must be a positive process ID.");
    if (!display && config_->platform.is_windows && pid > INT32_MAX) {
        auto summary = "\x1b[31;1mcapture.ps1: \x1b[31;1mCannot process argument transformation on parameter "
                       "'TargetPid'. Cannot convert value \"" +
                       std::to_string(pid) +
                       "\" to type \"System.Int32\". Error: \"Value was either too large or too small for an "
                       "Int32.\"\x1b[0m";
        if (environment("NO_COLOR") || env_or("TERM", "") == "dumb")
            summary = replace_all(replace_all(summary, "\x1b[31;1m", ""), "\x1b[0m", "");
        co_return result_process(summary, {}, "", summary + "\r\n", 1, false);
    }
    if (!display && pid > UINT32_MAX) {
        if (config_->platform.is_linux && !config_->platform.is_termux) {
            const auto session_type = lower(trim(env_or("XDG_SESSION_TYPE", "")));
            const bool wayland = environment("WAYLAND_DISPLAY").has_value() || session_type == "wayland";
            const bool x11 = environment("DISPLAY").has_value() || session_type == "x11";
            const auto session = wayland                ? "wayland"
                                 : x11                  ? "x11"
                                 : session_type.empty() ? "headless"
                                                        : session_type;
            const auto summary =
                !wayland && !x11 ? "No capturable Linux graphical session was detected (session=" + session +
                                       "). Set DISPLAY for X11 or WAYLAND_DISPLAY for Wayland and run Devbox "
                                       "inside the logged-in desktop session."
                : wayland && !x11
                    ? "No PID-selected window could be discovered on this Wayland compositor. Sway and "
                      "Hyprland are supported directly; other compositors may intentionally hide window/PID "
                      "enumeration and require an interactive desktop portal."
                    : "No visible X11/XWayland window was found for PID " + std::to_string(pid) +
                          " or its child processes.";
            co_return result_process(summary, {}, "", "", {}, false);
        }
        if (config_->platform.is_macos) {
            const auto script = fs::temp_directory_path() /
                                path_from_utf8("devbox-macos-window-capture-rust-" + std::to_string(pid)) /
                                "devbox-window-query.swift";
            co_return result_process(
                "No on-screen CoreGraphics window matched the requested process tree.",
                Json{{"file", "/usr/bin/swift"},
                     {"args", {path_text(script), "window", std::to_string(pid)}}},
                "", "No on-screen CoreGraphics window matched the requested process tree.\n", 3, false);
        }
        co_return result_process("Failed to capture " + config_->platform.display_name +
                                     " host window for PID " + std::to_string(pid) +
                                     ": pid exceeds the native process ID range.",
                                 {}, "", "", {}, false);
    }
    try {
        auto value =
            co_await capture_.capture(display ? std::nullopt : std::optional(static_cast<std::uint32_t>(pid)),
                                      static_cast<unsigned>(json_uint(args, "quality", 70)),
                                      json_bool(args, "include_process_tree", true), cancel);
        co_return result_image(display ? "Captured the " + lower(title) + " display."
                                       : "Captured " + title + " window for PID " + std::to_string(pid) + ".",
                               std::move(value.metadata), base64_encode(value.image),
                               std::move(value.mime_type));
    } catch (const std::exception& e) {
        co_return render_process_error(e, config_->command_output_limit_chars);
    }
}
} // namespace devbox
