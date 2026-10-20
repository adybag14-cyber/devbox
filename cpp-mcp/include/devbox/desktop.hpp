#pragma once
#include "process.hpp"
namespace devbox {
struct DesktopOptions {
    fs::path project, workspace, desktop_root;
    fs::path chromium = "/usr/lib/chromium/chromium";
    unsigned display = 99;
    bool shared = false, compatibility = false, browser_autostart = true;
};
DesktopOptions parse_desktop_options(const std::vector<std::string>& args);
std::vector<std::string> desktop_browser_arguments(const DesktopOptions& options);
Environment desktop_environment(const DesktopOptions& options);
int run_desktop_supervisor(const DesktopOptions& options, const std::function<bool()>& stop);
} // namespace devbox
