#include "devbox/desktop.hpp"
#include "devbox/browser_protocol.hpp"
#include "devbox/desktop_wm.hpp"
#include "devbox/native.hpp"
#include "devbox/state_store.hpp"
#include "devbox/storage.hpp"
#include "devbox/x11_session.hpp"
#include <array>
#include <charconv>
#include <future>
#include <iostream>
#include <map>
#include <sstream>
#include <system_error>
#include <thread>
#ifdef __linux__
#include <cerrno>
#include <fcntl.h>
#endif

namespace devbox {
namespace {
fs::path browser_profile(const DesktopOptions& options) {
    return options.shared ? options.desktop_root / "browser" : options.workspace / ".home" / "chromium";
}
std::string display_name(const DesktopOptions& options) {
    return ":" + std::to_string(options.display);
}
std::string start_ticks(std::uint32_t pid) {
#ifdef __linux__
    const auto path = "/proc/" + std::to_string(pid) + "/stat";
    int opened;
    do {
        opened = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    } while (opened < 0 && errno == EINTR);
    NativeHandle file(opened);
    if (!file) {
        const auto code = errno;
        throw std::system_error(code, std::generic_category(), "open " + path);
    }
    std::array<char, 8192> buffer{};
    std::size_t used = 0;
    while (used < buffer.size()) {
        const auto count = ::read(file.get(), buffer.data() + used, buffer.size() - used);
        if (count < 0) {
            const auto code = errno;
            if (code == EINTR)
                continue;
            throw std::system_error(code, std::generic_category(), "read " + path);
        }
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
    if (used == buffer.size())
        throw Error("DESKTOP_CHILD_IDENTITY_TOO_LARGE");
    const std::string text(buffer.data(), used);
    const auto end = text.rfind(')');
    if (end == std::string::npos)
        throw Error("DESKTOP_CHILD_IDENTITY");
    std::istringstream fields(text.substr(end + 1));
    std::string value;
    for (unsigned i = 0; i < 20; ++i)
        if (!(fields >> value))
            throw Error("DESKTOP_CHILD_IDENTITY");
    return value;
#else
    return std::to_string(process_instance(pid).value_or(0));
#endif
}
class DesktopLog {
    fs::path directory_;
    std::mutex mutex_;

  public:
    explicit DesktopLog(fs::path directory) : directory_(std::move(directory)) {}
    void append(const std::string& name, std::string_view data) {
        std::lock_guard lock(mutex_);
        const auto file = directory_ / (name + ".log");
        if (fs::exists(file) && fs::file_size(file) >= 1024 * 1024)
            replace_state_file(file, directory_ / (name + ".log.1"));
        write_file(file, data.substr(0, 32768), true);
    }
};
class DesktopChild {
    Cancel cancellation_ = std::make_shared<Cancellation>();
    std::future<ProcessOutput> work_;
    std::atomic<std::uint32_t> pid_{0};
    std::atomic<std::uint64_t> instance_{0};
    std::string ticks_;
    std::mutex identity_mutex_;
    fs::path program_;

  public:
    DesktopChild(const fs::path& program, const std::vector<std::string>& args, ProcessOptions process)
        : program_(fs::canonical(program)) {
        process.termination_grace = Millis(5000);
        process.max_capture_chars = 4096;
        process.on_pid = [this](std::uint32_t pid) {
            const auto instance = process_instance(pid);
            if (!instance)
                throw Error("DESKTOP_CHILD_IDENTITY");
            std::lock_guard lock(identity_mutex_);
            ticks_ = start_ticks(pid);
            instance_ = *instance;
            pid_ = pid;
        };
        work_ = std::async(std::launch::async, [this, args, process = std::move(process)] {
            return spawn_process(path_text(program_), args, process, cancellation_);
        });
    }
    ~DesktopChild() {
        stop();
    }
    bool running() const {
        return work_.valid() && work_.wait_for(Millis(0)) != std::future_status::ready;
    }
    bool started() const {
        return pid_.load() != 0 && instance_.load() != 0 && running();
    }
    std::uint32_t pid() const {
        return pid_.load();
    }
    Json identity(const std::string& name) {
        std::lock_guard lock(identity_mutex_);
        return {{"name", name},
                {"pid", pid_.load()},
                {"instance", std::to_string(instance_.load())},
                {"startTicks", ticks_},
                {"program", path_text(program_)},
                {"running", started()}};
    }
    void result() {
        if (work_.valid()) {
            const auto result = work_.get();
            if (result.exit_code)
                throw Error("DESKTOP_CHILD_EXIT_" + std::to_string(result.exit_code));
        }
    }
    Json exit_result() {
        if (!work_.valid() || running())
            return Json();
        try {
            const auto result = work_.get();
            return {{"exit_code", result.exit_code}, {"elapsed_ms", result.elapsed_ms}};
        } catch (const ProcessError& error) {
            return {{"exit_code", error.exit_code ? Json(*error.exit_code) : Json()},
                    {"signal", error.signal ? Json(*error.signal) : Json()},
                    {"timed_out", error.timed_out},
                    {"cancelled", error.aborted}};
        } catch (const Cancelled&) {
            return {{"cancelled", true}};
        } catch (...) {
            return {{"observation_error", true}};
        }
    }
    void stop() noexcept {
        cancellation_->cancel();
        if (work_.valid()) {
            try {
                (void)work_.get();
            } catch (...) {
            }
        }
    }
};
class Desktop {
    const DesktopOptions& options_;
    const std::function<bool()>& stop_;
    Environment environment_;
    fs::path logs_;
    DesktopLog log_;
    std::map<std::string, std::unique_ptr<DesktopChild>> children_;
    std::unique_ptr<DesktopWindowManager> window_manager_;
    std::uint64_t browser_restarts_ = 0, desktop_restarts_ = 0;
    unsigned browser_failures_ = 0;
    Clock::time_point browser_started_{};
    Json last_browser_exit_, browser_ready_at_;

    bool pause(Millis duration) const {
        const auto until = Clock::now() + duration;
        while (Clock::now() < until) {
            if (stop_())
                return false;
            std::this_thread::sleep_for(
                std::min(Millis(100), std::chrono::duration_cast<Millis>(until - Clock::now())));
        }
        return !stop_();
    }
    ProcessOptions process(const std::string& name) {
        ProcessOptions value;
        value.cwd = options_.workspace;
        value.env = environment_;
        value.max_capture_chars = 4096;
        value.on_output = [this, name](OutputStream, std::string_view data) { log_.append(name, data); };
        return value;
    }
    fs::path program(std::string_view name) const {
        const auto value = find_program(name, &environment_);
        if (!value)
            throw Error("DESKTOP_PROGRAM_UNAVAILABLE: " + std::string(name));
        return fs::canonical(*value);
    }
    void child(const std::string& name, const fs::path& file, const std::vector<std::string>& args) {
        if (children_.contains(name))
            throw Error("DESKTOP_DUPLICATE_CHILD");
        auto value = std::make_unique<DesktopChild>(file, args, process(name));
        const auto until = Clock::now() + Millis(5000);
        while (!value->started()) {
            if (!value->running()) {
                value->result();
                throw Error("DESKTOP_CHILD_EXIT_BEFORE_READY: " + name);
            }
            if (Clock::now() >= until || !pause(Millis(10)))
                throw Error("DESKTOP_CHILD_START_DEADLINE: " + name);
        }
        children_.emplace(name, std::move(value));
    }
    void stop_child(const std::string& name) {
        const auto found = children_.find(name);
        if (found != children_.end()) {
            found->second->stop();
            children_.erase(found);
        }
    }
    bool alive(const std::string& name) const {
        const auto found = children_.find(name);
        return found != children_.end() && found->second->started();
    }
    void status(bool ready, const std::string& error = {}) {
        Json children = Json::array();
        for (auto& [name, value] : children_)
            children.push_back(value->identity(name));
        write_json_atomic(
            logs_ / "status.json",
            {{"ready", ready},
             {"updatedAt", utc_now()},
             {"display", display_name(options_)},
             {"implementation", "cpp"},
             {"supervisor_pid", process_id()},
             {"browser", options_.browser_autostart ? "headed Chromium" : "on-demand native browser"},
             {"backend", "native C++23 XCB/XTEST"},
             {"window_manager", "native C++23"},
             {"window_manager_ready", window_manager_ && window_manager_->healthy()},
             {"session_bus", "disabled"},
             {"browser_mode", options_.shared              ? "shared_native"
                              : options_.browser_autostart ? "desktop"
                                                           : "on_demand"},
             {"browser_profile", path_text(browser_profile(options_))},
             {"children", std::move(children)},
             {"browser_restarts", browser_restarts_},
             {"desktop_restarts", desktop_restarts_},
             {"last_browser_exit", last_browser_exit_},
             {"browser_ready_at", browser_ready_at_},
             {"error", error.substr(0, 1024)}});
    }
    void start_browser() {
        const auto profile = browser_profile(options_);
        if (!options_.shared && !fs::exists(profile.parent_path()))
            ensure_private_state_directory(profile.parent_path());
        ensure_private_state_directory(profile);
        if (options_.shared) {
            std::error_code ec;
            fs::remove(profile / "DevToolsActivePort", ec);
            if (ec)
                throw Error("DESKTOP_ENDPOINT_CLEANUP");
        }
        child("chromium", options_.chromium, desktop_browser_arguments(options_));
        browser_started_ = Clock::now();
        browser_ready_at_ = Json();
        if (!options_.shared)
            return;
        status(false, "Managed browser is warming");
        const auto until = Clock::now() + Millis(180000);
        std::string last_error = "Endpoint has not been published";
        while (!stop_()) {
            if (!alive("chromium"))
                throw Error("DESKTOP_BROWSER_EXITED_DURING_STARTUP");
            const auto endpoint_file = profile / "DevToolsActivePort";
            if (fs::exists(endpoint_file)) {
                if (fs::is_symlink(fs::symlink_status(endpoint_file)))
                    throw Error("DESKTOP_ENDPOINT_SYMLINK");
                try {
                    const auto lines = split(trim(read_file(endpoint_file, 1024)), '\n');
                    unsigned port = 0;
                    if (lines.size() != 2)
                        throw Error("DESKTOP_ENDPOINT_INVALID");
                    const auto parsed =
                        std::from_chars(lines[0].data(), lines[0].data() + lines[0].size(), port);
                    if (parsed.ec != std::errc{} || parsed.ptr != lines[0].data() + lines[0].size() ||
                        !port || port > 65535)
                        throw Error("DESKTOP_ENDPOINT_INVALID");
                    BrowserProtocol probe(65536);
                    const auto deadline = std::min(until, Clock::now() + Millis(2000));
                    probe.connect(static_cast<std::uint16_t>(port), trim(lines[1]), deadline);
                    const auto version = probe.call("Browser.getVersion", Json::object(), deadline);
                    if (json_string(version, "product").empty() ||
                        json_string(version, "protocolVersion").empty())
                        throw Error("DESKTOP_BROWSER_NOT_READY");
                    browser_ready_at_ = utc_now();
                    return;
                } catch (const Error& error) {
                    last_error = std::string(error.what()).substr(0, 256);
                }
            }
            if (Clock::now() >= until)
                throw Error("DESKTOP_BROWSER_STARTUP_DEADLINE: " + last_error);
            pause(Millis(200));
        }
        throw Cancelled();
    }
    void start_desktop() {
        const auto authority = options_.desktop_root / "authority";
        X11Cookie cookie;
        const auto bytes = random_bytes(cookie.size());
        std::copy(bytes.begin(), bytes.end(), cookie.begin());
        write_xauthority(authority, options_.display, cookie);
        child("xvfb", program("Xvfb"),
              {display_name(options_), "-screen", "0", "1280x800x24", "-nolisten", "tcp", "-auth",
               path_text(authority), "-noreset"});
        const auto xvfb_pid = children_.at("xvfb")->pid();
        const auto until = Clock::now() + Millis(20000);
        for (;;) {
            if (!alive("xvfb") || stop_())
                throw Error("DESKTOP_XVFB_EXITED_BEFORE_READY");
            try {
                if (probe_private_x11(options_.display, cookie, std::min(until, Clock::now() + Millis(1000)),
                                      stop_, xvfb_pid))
                    break;
            } catch (const std::runtime_error&) {
            }
            if (Clock::now() >= until || !pause(Millis(200)))
                throw Error("DESKTOP_X11_STARTUP_DEADLINE");
        }
        window_manager_ = std::make_unique<DesktopWindowManager>(options_.display, cookie, stop_, xvfb_pid);
        if (options_.browser_autostart)
            start_browser();
    }

  public:
    Desktop(const DesktopOptions& options, const std::function<bool()>& stop)
        : options_(options), stop_(stop), environment_(desktop_environment(options)),
          logs_(options.project / "run" / "desktop"), log_(logs_) {}
    ~Desktop() {
        stop_children();
    }
    void stop_children() {
        stop_child("chromium");
        window_manager_.reset();
        stop_child("xvfb");
    }
    void run() {
        while (!stop_()) {
            try {
                start_desktop();
                status(true);
                auto next_status = Clock::now() + Millis(5000);
                while (!stop_()) {
                    if (!alive("xvfb"))
                        throw Error("DESKTOP_CHILD_EXITED: xvfb");
                    if (!window_manager_ || !window_manager_->healthy())
                        throw Error("DESKTOP_WINDOW_MANAGER_EXITED_OR_STALLED");
                    if (options_.browser_autostart && !alive("chromium")) {
                        last_browser_exit_ = children_.at("chromium")->exit_result();
                        if (last_browser_exit_.is_object())
                            last_browser_exit_["observed_at"] = utc_now();
                        if (Clock::now() - browser_started_ >= Millis(60000))
                            browser_failures_ = 0;
                        const auto delay = std::min(60000U, 1500U << std::min(browser_failures_, 6U));
                        browser_failures_ = std::min(browser_failures_ + 1, 7U);
                        status(false, "Managed browser exited; restarting owned child");
                        stop_child("chromium");
                        ++browser_restarts_;
                        if (!pause(Millis(delay)))
                            break;
                        start_browser();
                        status(true);
                    }
                    if (Clock::now() >= next_status) {
                        status(true);
                        next_status = Clock::now() + Millis(5000);
                    }
                    pause(Millis(500));
                }
            } catch (const std::exception& error) {
                status(false, error.what());
                std::cerr << error.what() << '\n';
            }
            stop_children();
            if (!stop_()) {
                ++desktop_restarts_;
                pause(Millis(3000));
            }
        }
        status(false, "Desktop supervisor stopped");
    }
};
} // namespace

DesktopOptions parse_desktop_options(const std::vector<std::string>& args) {
    DesktopOptions result;
    result.project = fs::current_path();
    result.workspace = path_from_utf8(env_or("HOST_WORKSPACE_PATH", "/workspace"));
    result.desktop_root = "/tmp/devbox-desktop";
    result.shared = env_bool("DEVBOX_BROWSER_SHARED", false);
    result.compatibility = env_bool("DEVBOX_BROWSER_COMPATIBILITY", false);
    result.browser_autostart = result.shared || env_or("DEVBOX_DESKTOP_BROWSER_AUTOSTART", "1") != "0";
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& flag = args[i];
        if (++i == args.size())
            throw Error("Desktop option requires a value: " + flag);
        const auto& value = args[i];
        if (flag == "--project-root")
            result.project = path_from_utf8(value);
        else if (flag == "--workspace")
            result.workspace = path_from_utf8(value);
        else if (flag == "--desktop-root")
            result.desktop_root = path_from_utf8(value);
        else if (flag == "--chromium")
            result.chromium = path_from_utf8(value);
        else if (flag == "--display") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.display);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                result.display > 65535)
                throw Error("DESKTOP_DISPLAY_INVALID");
        } else
            throw Error("Unknown desktop option: " + flag);
    }
    for (auto* path : {&result.project, &result.workspace, &result.desktop_root, &result.chromium}) {
        *path = path->lexically_normal();
        if (!path->is_absolute() || *path == path->root_path())
            throw Error("DESKTOP_ABSOLUTE_NONROOT_PATH_REQUIRED");
    }
    return result;
}
std::vector<std::string> desktop_browser_arguments(const DesktopOptions& options) {
    std::vector<std::string> args{"--ozone-platform=x11",
                                  "--disable-gpu",
                                  "--no-first-run",
                                  "--no-default-browser-check",
                                  "--disable-background-networking",
                                  "--disable-component-update",
                                  "--disable-sync",
                                  "--disable-features=Translate,MediaRouter,OptimizationHints",
                                  "--renderer-process-limit=2",
                                  "--js-flags=--max-old-space-size=128",
                                  "--window-size=1200,740",
                                  "--window-position=20,20",
                                  "--user-data-dir=" + path_text(browser_profile(options))};
    if (options.shared && options.compatibility)
        args.emplace_back("--disable-blink-features=AutomationControlled");
    if (options.shared)
        for (const auto* flag :
             {"--remote-debugging-address=127.0.0.1", "--remote-debugging-port=0", "--disable-extensions",
              "--disable-quic", "--force-webrtc-ip-handling-policy=disable_non_proxied_udp",
              "--proxy-server=http://127.0.0.1:9", "--proxy-bypass-list=<-loopback>",
              "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1"})
            args.emplace_back(flag);
    args.emplace_back("about:blank");
    return args;
}
Environment desktop_environment(const DesktopOptions& options) {
    auto env = worker_environment();
    env["HOME"] = path_text(options.workspace / ".home");
    env["DISPLAY"] = display_name(options);
    env["XAUTHORITY"] = path_text(options.desktop_root / "authority");
    env["XDG_RUNTIME_DIR"] = path_text(options.desktop_root / "runtime");
    // Devbox owns no D-Bus service. Explicitly disable auto-launch and host-bus
    // discovery rather than starting an idle daemon for Chromium.
    env["DBUS_SESSION_BUS_ADDRESS"] =
        "unix:path=" + path_text(options.desktop_root / "runtime" / "bus-disabled");
    env["DBUS_SYSTEM_BUS_ADDRESS"] = env.at("DBUS_SESSION_BUS_ADDRESS");
    env.erase("SESSION_MANAGER");
    env.erase("WAYLAND_DISPLAY");
    env.erase("DBUS_STARTER_ADDRESS");
    env.erase("DBUS_STARTER_BUS_TYPE");
    return env;
}
int run_desktop_supervisor(const DesktopOptions& options, const std::function<bool()>& stop) {
#if !defined(__linux__) || defined(__ANDROID__)
    (void)options;
    (void)stop;
    throw Error("Native desktop supervision requires Linux");
#else
    if (!fs::is_directory(options.project) || !fs::is_directory(options.workspace))
        throw Error("DESKTOP_PROJECT_AND_WORKSPACE_REQUIRED");
    if (!fs::exists(options.workspace / ".home"))
        ensure_private_state_directory(options.workspace / ".home");
    ensure_private_state_directory(options.desktop_root);
    ensure_private_state_directory(options.desktop_root / "runtime");
    ensure_private_state_directory(options.project / "run");
    ensure_private_state_directory(options.project / "run" / "desktop");
    FileLock owned(options.desktop_root / "supervisor.lock", Millis(100), {}, true);
    // A former supervisor may have died without collecting its children.
    // Never overwrite their ownership evidence or adopt/kill an arbitrary PID.
    const auto status_file = options.project / "run" / "desktop" / "status.json";
    if (fs::is_symlink(fs::symlink_status(status_file)))
        throw Error("DESKTOP_STATUS_SYMLINK");
    if (const auto prior = read_json_optional(status_file, 16384)) {
        for (const auto& child : prior->value("children", Json::array())) {
            const auto pid = json_uint(child, "pid");
            if (!pid || pid > UINT32_MAX)
                continue;
            std::string actual;
            try {
                actual = start_ticks(static_cast<std::uint32_t>(pid));
            } catch (const std::system_error& error) {
                // A former child may already have exited, including between
                // reading the status record and opening its proc entry.
                // Permission and other I/O failures must still fail closed.
                if (error.code() == std::errc::no_such_file_or_directory ||
                    error.code() == std::errc::no_such_process)
                    continue;
                throw;
            }
            if (actual == json_string(child, "startTicks"))
                throw Error("DESKTOP_LIVE_CHILDREN_REQUIRE_OPERATOR_REVIEW");
        }
    }
    Desktop desktop(options, stop);
    desktop.run();
    return 0;
#endif
}
} // namespace devbox
