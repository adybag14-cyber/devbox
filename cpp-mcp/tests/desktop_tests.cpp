#include "devbox/desktop.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <iostream>
using namespace devbox;
int main() {
    try {
        DesktopOptions options;
        options.project = fs::temp_directory_path() / "desktop-fixture";
        options.workspace = options.project / "workspace";
        options.desktop_root = options.project / "private";
        options.shared = true;
        options.compatibility = true;
        const auto prior = environment("DESKTOP_PRIVATE_CREDENTIAL_SENTINEL");
        ScopeExit restore([&] { set_environment("DESKTOP_PRIVATE_CREDENTIAL_SENTINEL", prior); });
        set_environment("DESKTOP_PRIVATE_CREDENTIAL_SENTINEL", "never inherit");
        const auto env = desktop_environment(options);
        if (env.contains("DESKTOP_PRIVATE_CREDENTIAL_SENTINEL") || env.contains("SESSION_MANAGER") ||
            env.contains("WAYLAND_DISPLAY") || env.at("HOME") != path_text(options.workspace / ".home") ||
            env.at("XAUTHORITY") != path_text(options.desktop_root / "authority"))
            throw Error("Native desktop environment isolation failed");
        const auto args = desktop_browser_arguments(options);
        if (std::find(args.begin(), args.end(), "--no-sandbox") != args.end() ||
            std::find(args.begin(), args.end(), "--remote-debugging-address=127.0.0.1") == args.end() ||
            std::find(args.begin(), args.end(), "--proxy-server=http://127.0.0.1:9") == args.end() ||
            std::find(args.begin(), args.end(), "--disable-blink-features=AutomationControlled") ==
                args.end() ||
            std::find(args.begin(), args.end(),
                      "--user-data-dir=" + path_text(options.desktop_root / "browser")) == args.end())
            throw Error("Managed Chromium must retain sandbox and closed default egress");
        bool rejected = false;
        try {
            (void)parse_desktop_options({"--display", "99;unexpected"});
        } catch (const Error&) {
            rejected = true;
        }
        if (!rejected)
            throw Error("Desktop display must be numeric");
        std::cout << "native desktop environment and launch boundaries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
