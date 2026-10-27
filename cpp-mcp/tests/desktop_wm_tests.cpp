#include "desktop_wm_proxy.hpp"
#include "devbox/desktop_wm.hpp"
#include <algorithm>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <set>
#include <stdexcept>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
using namespace devbox;
namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct SigpipeBaseline {
    struct sigaction previous{};
    sigset_t previous_mask{};
    SigpipeBaseline() {
        require(::sigaction(SIGPIPE, nullptr, &previous) == 0 &&
                    ::pthread_sigmask(SIG_SETMASK, nullptr, &previous_mask) == 0,
                "Read inherited SIGPIPE policy");
        sigset_t pending;
        require(::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 0,
                "Fixture inherited a pending SIGPIPE");
        struct sigaction fatal{};
        fatal.sa_handler = SIG_DFL;
        ::sigemptyset(&fatal.sa_mask);
        require(::sigaction(SIGPIPE, &fatal, nullptr) == 0, "Establish fatal SIGPIPE baseline");
        sigset_t pipe;
        ::sigemptyset(&pipe);
        ::sigaddset(&pipe, SIGPIPE);
        if (::pthread_sigmask(SIG_UNBLOCK, &pipe, nullptr) != 0) {
            ::sigaction(SIGPIPE, &previous, nullptr);
            throw std::runtime_error("Unblock fixture SIGPIPE baseline");
        }
    }
    ~SigpipeBaseline() {
        ::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
        ::sigaction(SIGPIPE, &previous, nullptr);
    }
};
template <typename F> void until(F&& condition, const char* message) {
    const auto end = Clock::now() + std::chrono::seconds(2);
    while (!condition()) {
        require(Clock::now() < end, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
struct Connection {
    xcb_connection_t* value;
    ~Connection() {
        xcb_disconnect(value);
    }
};
struct Free {
    void operator()(void* value) const {
        std::free(value);
    }
};
template <typename T> using Reply = std::unique_ptr<T, Free>;
std::size_t descriptors() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), {}));
}
std::set<int> window_manager_threads() {
    std::set<int> result;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream comm(entry.path() / "comm");
        std::string name;
        if (std::getline(comm, name) && name == "devbox-x11-wm")
            result.insert(std::stoi(entry.path().filename().string()));
    }
    return result;
}
} // namespace
void check_window_manager(unsigned display, const X11Cookie& cookie, int server_pid) {
    const SigpipeBaseline signal_baseline;
    const auto baseline = descriptors();
    {
        const auto original_threads = window_manager_threads();
        struct sigaction original_signal{};
        require(::sigaction(SIGPIPE, nullptr, &original_signal) == 0, "Read original SIGPIPE policy");
        auto wm = std::make_unique<DesktopWindowManager>(display, cookie, [] { return false; });
        require(wm->healthy(), "Native window manager not ready");
        // Sanitizers may create their own helper thread during the first
        // std::thread construction. Select our explicitly named worker only.
        auto owned_threads = window_manager_threads();
        for (const auto thread : original_threads)
            owned_threads.erase(thread);
        require(owned_threads.size() == 1, "Native window manager must own exactly one worker");
        // The real cancellation race can produce SIGPIPE while XCB is writing
        // the handshake. Deliver it deterministically to that exact owned
        // worker; normal X11 operations and teardown must still succeed.
        require(::syscall(SYS_tgkill, ::getpid(), *owned_threads.begin(), SIGPIPE) == 0,
                "Deliver SIGPIPE to owned window-manager worker");
        struct sigaction current_signal{};
        require(::sigaction(SIGPIPE, nullptr, &current_signal) == 0 &&
                    current_signal.sa_handler == original_signal.sa_handler,
                "Window manager changed process-wide SIGPIPE policy");
        sigset_t creating_mask;
        require(::pthread_sigmask(SIG_SETMASK, nullptr, &creating_mask) == 0 &&
                    ::sigismember(&creating_mask, SIGPIPE) == 0,
                "Window manager changed its creating thread's SIGPIPE mask");
        bool duplicate_rejected = false;
        try {
            DesktopWindowManager duplicate(display, cookie, [] { return false; });
        } catch (const std::exception& error) {
            duplicate_rejected =
                std::string(error.what()).find("EXISTING_WINDOW_MANAGER") != std::string::npos;
        }
        require(duplicate_rejected && wm->healthy(), "A second window manager stole the private display");
        xcb_auth_info_t auth{18, const_cast<char*>("MIT-MAGIC-COOKIE-1"), 16,
                             reinterpret_cast<char*>(const_cast<std::uint8_t*>(cookie.data()))};
        Connection client{xcb_connect_to_fd(
            connect_private_x11(display, Clock::now() + std::chrono::seconds(1), {}), &auth)};
        require(!xcb_connection_has_error(client.value), "Independent XCB test client failed");
        auto* screen = xcb_setup_roots_iterator(xcb_get_setup(client.value)).data;
        const auto atom = [&](const char* name) {
            Reply<xcb_intern_atom_reply_t> value(xcb_intern_atom_reply(
                client.value,
                xcb_intern_atom(client.value, false, static_cast<std::uint16_t>(std::strlen(name)), name),
                nullptr));
            require(!!value, "Fixture atom unavailable");
            return value->atom;
        };
        const auto property = [&](xcb_window_t window, const char* name) {
            Reply<xcb_get_property_reply_t> value(xcb_get_property_reply(
                client.value,
                xcb_get_property(client.value, false, window, atom(name), XCB_GET_PROPERTY_TYPE_ANY, 0, 1024),
                nullptr));
            require(!!value, "Fixture property unavailable");
            std::vector<std::uint32_t> result(value->format == 32 ? value->value_len : 0);
            if (!result.empty())
                std::memcpy(result.data(), xcb_get_property_value(value.get()), result.size() * 4);
            return result;
        };
        const auto message = [&](xcb_window_t window, const char* type, std::uint32_t first,
                                 std::uint32_t second = 0, std::uint32_t third = 0) {
            xcb_client_message_event_t event{};
            event.response_type = XCB_CLIENT_MESSAGE;
            event.format = 32;
            event.window = window;
            event.type = atom(type);
            event.data.data32[0] = first;
            event.data.data32[1] = second;
            event.data.data32[2] = third;
            xcb_send_event(client.value, false, screen->root,
                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                           reinterpret_cast<const char*>(&event));
            xcb_flush(client.value);
        };
        const auto geometry = [&](xcb_window_t window) {
            Reply<xcb_get_geometry_reply_t> value(
                xcb_get_geometry_reply(client.value, xcb_get_geometry(client.value, window), nullptr));
            require(!!value, "Fixture geometry unavailable");
            return *value;
        };
        const auto mapped = [&](xcb_window_t window) {
            Reply<xcb_get_window_attributes_reply_t> value(xcb_get_window_attributes_reply(
                client.value, xcb_get_window_attributes(client.value, window), nullptr));
            require(!!value, "Fixture attributes unavailable");
            return value->map_state == XCB_MAP_STATE_VIEWABLE;
        };
        const auto make = [&](bool override_redirect) {
            const auto window = xcb_generate_id(client.value);
            const std::uint32_t values[]{override_redirect ? 1u : 0u,
                                         XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_FOCUS_CHANGE};
            xcb_create_window(client.value, XCB_COPY_FROM_PARENT, window, screen->root, 20, 30, 300, 200, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                              XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
            const auto close = atom("WM_DELETE_WINDOW");
            xcb_change_property(client.value, XCB_PROP_MODE_REPLACE, window, atom("WM_PROTOCOLS"),
                                XCB_ATOM_ATOM, 32, 1, &close);
            xcb_map_window(client.value, window);
            xcb_flush(client.value);
            return window;
        };
        const auto first = make(false), second = make(false);
        until(
            [&] {
                return property(screen->root, "_NET_CLIENT_LIST") ==
                           std::vector<std::uint32_t>{first, second} &&
                       mapped(first) && mapped(second);
            },
            "Window map/client order failed");
        require(property(first, "_NET_FRAME_EXTENTS") == std::vector<std::uint32_t>({0, 0, 0, 0}),
                "Native frame extents must describe borderless geometry");
        message(first, "_NET_ACTIVE_WINDOW", 1);
        until(
            [&] { return property(screen->root, "_NET_ACTIVE_WINDOW") == std::vector<std::uint32_t>{first}; },
            "Native activation failed");
        {
            Reply<xcb_get_input_focus_reply_t> focus(
                xcb_get_input_focus_reply(client.value, xcb_get_input_focus(client.value), nullptr));
            require(focus && focus->focus == first, "Native focus did not reach client");
            require(property(screen->root, "_NET_CLIENT_LIST_STACKING").back() == first,
                    "Native stacking did not follow activation");
        }
        const std::uint32_t size[]{100, 80, 320, 210};
        xcb_configure_window(client.value, first,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                 XCB_CONFIG_WINDOW_HEIGHT,
                             size);
        xcb_flush(client.value);
        until(
            [&] {
                const auto box = geometry(first);
                return box.x == 100 && box.y == 80 && box.width == 320 && box.height == 210;
            },
            "Native configure request failed");
        message(first, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_FULLSCREEN"));
        until(
            [&] {
                const auto box = geometry(first);
                return box.x == 0 && box.y == 0 && box.width == 640 && box.height == 480;
            },
            "Fullscreen geometry failed");
        message(first, "_NET_WM_STATE", 0, atom("_NET_WM_STATE_FULLSCREEN"));
        until(
            [&] {
                const auto box = geometry(first);
                return box.x == 100 && box.y == 80 && box.width == 320 && box.height == 210;
            },
            "Fullscreen restore failed");
        message(first, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
                atom("_NET_WM_STATE_MAXIMIZED_VERT"));
        until(
            [&] {
                const auto box = geometry(first);
                return box.width == 640 && box.height == 480;
            },
            "Maximize failed");
        message(first, "_NET_WM_STATE", 0, atom("_NET_WM_STATE_MAXIMIZED_HORZ"),
                atom("_NET_WM_STATE_MAXIMIZED_VERT"));
        until([&] { return geometry(first).width == 320; }, "Maximize restore failed");
        message(first, "WM_CHANGE_STATE", 3);
        until(
            [&] {
                return !mapped(first) && property(first, "WM_STATE") == std::vector<std::uint32_t>({3, 0});
            },
            "Minimize failed");
        require(property(screen->root, "_NET_CLIENT_LIST").size() == 2,
                "Minimized window lost from client list");
        message(first, "_NET_ACTIVE_WINDOW", 1);
        until(
            [&] {
                return mapped(first) && property(first, "WM_STATE") == std::vector<std::uint32_t>({1, 0});
            },
            "Restore minimized window failed");
        const auto popup = make(true);
        until([&] { return mapped(popup); }, "Override-redirect popup failed");
        require(property(screen->root, "_NET_CLIENT_LIST").size() == 2,
                "Native manager adopted an override-redirect popup");
        message(first, "_NET_CLOSE_WINDOW", 0);
        until(
            [&] {
                for (;;) {
                    Reply<xcb_generic_event_t> event(xcb_poll_for_event(client.value));
                    if (!event)
                        return false;
                    if ((event->response_type & 127) == XCB_CLIENT_MESSAGE) {
                        const auto* value = reinterpret_cast<xcb_client_message_event_t*>(event.get());
                        if (value->window == first && value->type == atom("WM_PROTOCOLS") &&
                            value->data.data32[0] == atom("WM_DELETE_WINDOW"))
                            return true;
                    }
                }
            },
            "Close request did not use WM_DELETE_WINDOW");
        xcb_destroy_window(client.value, first);
        xcb_flush(client.value);
        until(
            [&] { return property(screen->root, "_NET_CLIENT_LIST") == std::vector<std::uint32_t>{second}; },
            "Destroyed client retained");
        require(wm->healthy(), "Native manager did not survive window lifecycle");
        const auto stop_start = Clock::now();
        wm.reset();
        require(Clock::now() - stop_start < std::chrono::seconds(1), "Native WM shutdown exceeded bound");
    }
    require(descriptors() == baseline, "Window manager leaked descriptors after close or duplicate refusal");
    {
        SlowX11Relay relay(display, static_cast<std::uint32_t>(server_pid));
        DesktopWindowManager wm(
            relay.display(), cookie, [] { return false; }, static_cast<std::uint32_t>(::getpid()));
        xcb_auth_info_t auth{18, const_cast<char*>("MIT-MAGIC-COOKIE-1"), 16,
                             reinterpret_cast<char*>(const_cast<std::uint8_t*>(cookie.data()))};
        Connection client{xcb_connect_to_fd(
            connect_private_x11(display, Clock::now() + std::chrono::seconds(1), {}), &auth)};
        require(!xcb_connection_has_error(client.value), "Slow-reply fixture client");
        const auto screen = xcb_setup_roots_iterator(xcb_get_setup(client.value)).data;
        const auto window = xcb_generate_id(client.value);
        xcb_create_window(client.value, XCB_COPY_FROM_PARENT, window, screen->root, 10, 10, 300, 200, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        relay.delay(700);
        const auto start = Clock::now();
        xcb_map_window(client.value, window);
        xcb_flush(client.value);
        bool mapped = false;
        while (!mapped || Clock::now() - start < std::chrono::seconds(7)) {
            require(Clock::now() - start < std::chrono::seconds(18), "Slow progressive map deadline");
            require(!relay.failed(), "Private slow X11 relay failed");
            require(wm.healthy(), "Progressing X11 replies were misclassified as a stalled window manager");
            Reply<xcb_get_window_attributes_reply_t> attributes(xcb_get_window_attributes_reply(
                client.value, xcb_get_window_attributes(client.value, window), nullptr));
            require(!!attributes, "Slow-reply fixture attributes");
            mapped = attributes->map_state == XCB_MAP_STATE_VIEWABLE;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        relay.delay(0);
        std::cout << "Native WM: replies progressing for over five seconds remain healthy\n";
    }
    require(descriptors() == baseline, "Slow-reply WM fixture leaked descriptors");
    {
        SlowX11Relay relay(display, static_cast<std::uint32_t>(server_pid));
        DesktopWindowManager wm(
            relay.display(), cookie, [] { return false; }, static_cast<std::uint32_t>(::getpid()));
        xcb_auth_info_t auth{18, const_cast<char*>("MIT-MAGIC-COOKIE-1"), 16,
                             reinterpret_cast<char*>(const_cast<std::uint8_t*>(cookie.data()))};
        Connection client{xcb_connect_to_fd(
            connect_private_x11(display, Clock::now() + std::chrono::seconds(1), {}), &auth)};
        const auto screen = xcb_setup_roots_iterator(xcb_get_setup(client.value)).data;
        const auto window = xcb_generate_id(client.value);
        xcb_create_window(client.value, XCB_COPY_FROM_PARENT, window, screen->root, 10, 10, 200, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        // Deliver the map event immediately, then withhold its first requested
        // reply. An idle event loop is not a stalled request.
        relay.delay_window_attributes(window, 5000);
        const auto start = Clock::now();
        xcb_map_window(client.value, window);
        xcb_flush(client.value);
        while (wm.healthy() && Clock::now() - start < std::chrono::seconds(4))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(relay.selected_delay_triggered() && !wm.healthy() &&
                    wm.failure_reason() == "DESKTOP_WINDOW_MANAGER_REPLY_DEADLINE",
                "A stalled X11 reply escaped its deadline or lost the diagnostic reason");
        std::cout << "Native WM: nonprogressing reply still fails within its bounded deadline\n";
    }
    require(descriptors() == baseline, "Stalled-reply WM fixture leaked descriptors");
    // Stall only our still-unreaped Xvfb child. Cancellation must unblock XCB's
    // synchronous setup using the pinned socket and leave no worker behind.
    require(::kill(server_pid, SIGSTOP) == 0, "Could not suspend owned Xvfb fixture");
    struct Resume {
        int pid;
        ~Resume() {
            ::kill(pid, SIGCONT);
        }
    } resume{server_pid};
    const auto started = Clock::now();
    bool cancelled = false;
    try {
        DesktopWindowManager blocked(
            display, cookie, [&] { return Clock::now() - started >= std::chrono::milliseconds(150); });
    } catch (const std::exception&) {
        cancelled = true;
    }
    require(cancelled && Clock::now() - started < std::chrono::seconds(2),
            "Blocked XCB setup ignored cancellation");
    require(descriptors() == baseline, "Cancelled WM setup leaked descriptors");
    std::cout << "Native WM: independent map, focus, stacking, configure, fullscreen, restore, close, "
                 "ownership and stalled-server cancellation passed\n";
}
