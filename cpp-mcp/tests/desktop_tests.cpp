#include "devbox/desktop.hpp"
#include "devbox/native.hpp"
#include "devbox/storage.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/wait.h>
#include <unistd.h>
#endif
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
            env.at("XAUTHORITY") != path_text(options.desktop_root / "authority") ||
            env.at("DBUS_SESSION_BUS_ADDRESS") !=
                "unix:path=" + path_text(options.desktop_root / "runtime" / "bus-disabled") ||
            env.at("DBUS_SYSTEM_BUS_ADDRESS") != env.at("DBUS_SESSION_BUS_ADDRESS") ||
            env.contains("DBUS_STARTER_ADDRESS") || env.contains("DBUS_STARTER_BUS_TYPE"))
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
#if defined(__linux__) && !defined(__ANDROID__)
        // Exercise the actual proc-file lookup with a child that has exited
        // and been reaped. An already-stopped supervisor needs no X11 tools.
        options.project = fs::temp_directory_path() / ("desktop-recovery-" + uuid());
        options.workspace = options.project / "workspace";
        options.desktop_root = options.project / "private";
        fs::create_directories(options.workspace);
        ScopeExit remove_fixture([&] { fs::remove_all(options.project); });
        const auto exited_pid = fork();
        if (exited_pid < 0)
            throw Error("Could not create exited desktop fixture");
        if (exited_pid == 0)
            _exit(0);
        int exit_status = 0;
        if (waitpid(exited_pid, &exit_status, 0) != exited_pid || !WIFEXITED(exit_status) ||
            WEXITSTATUS(exit_status) != 0)
            throw Error("Desktop fixture child did not exit normally");
        if (fs::exists("/proc/" + std::to_string(exited_pid)))
            throw Error("Exited desktop fixture PID was unexpectedly reused");
        const auto status_file = options.project / "run" / "desktop" / "status.json";
        fs::create_directories(status_file.parent_path());
        fs::permissions(options.project / "run", fs::perms::owner_all);
        fs::permissions(status_file.parent_path(), fs::perms::owner_all);
        write_json_atomic(status_file,
                          {{"children", Json::array({{{"pid", exited_pid}, {"startTicks", "1"}}})}});
        if (run_desktop_supervisor(options, [] { return true; }) != 0)
            throw Error("Missing former desktop child prevented recovery");

        // A matching live identity must still be rejected without overwriting
        // its record. A reused PID with a different birth time is safe to ignore.
        const auto stat = read_file("/proc/self/stat", 8192);
        std::istringstream fields(stat.substr(stat.rfind(')') + 1));
        std::string ticks;
        for (unsigned i = 0; i < 20; ++i)
            if (!(fields >> ticks))
                throw Error("Could not read live desktop fixture identity");
        const Json live = {{"children", Json::array({{{"pid", process_id()}, {"startTicks", ticks}}})}};
        write_json_atomic(status_file, live);
        rejected = false;
        try {
            (void)run_desktop_supervisor(options, [] { return true; });
        } catch (const Error& error) {
            rejected = std::string(error.what()) == "DESKTOP_LIVE_CHILDREN_REQUIRE_OPERATOR_REVIEW";
        }
        if (!rejected || read_json(status_file) != live)
            throw Error("Live desktop ownership was not preserved");
        write_json_atomic(status_file,
                          {{"children", Json::array({{{"pid", process_id()}, {"startTicks", "0"}}})}});
        if (run_desktop_supervisor(options, [] { return true; }) != 0)
            throw Error("Reused desktop PID prevented recovery");
#endif
        std::cout << "native desktop environment and launch boundaries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
