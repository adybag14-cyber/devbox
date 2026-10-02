#include "devbox/computer_x11.hpp"
#include "devbox/native.hpp"
#if defined(__linux__) && !defined(__ANDROID__)
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xtest.h>
#include <zlib.h>

namespace devbox {
namespace {
void check(const Cancel& cancel) {
    if (cancel)
        cancel->check();
}
void delay(unsigned ms, const Cancel& cancel) {
    const auto until = Clock::now() + Millis(ms);
    while (Clock::now() < until) {
        check(cancel);
        std::this_thread::sleep_for(
            std::min(Millis(10), std::chrono::duration_cast<Millis>(until - Clock::now())));
    }
    check(cancel);
}
struct Api {
    void* core = nullptr;
    void* test = nullptr;
#define XCB_FUNCTION(name) decltype(&::xcb_##name) name = nullptr
    XCB_FUNCTION(connect);
    XCB_FUNCTION(disconnect);
    XCB_FUNCTION(connection_has_error);
    XCB_FUNCTION(get_file_descriptor);
    XCB_FUNCTION(get_setup);
    XCB_FUNCTION(setup_roots_iterator);
    XCB_FUNCTION(screen_next);
    XCB_FUNCTION(setup_pixmap_formats_iterator);
    XCB_FUNCTION(format_next);
    XCB_FUNCTION(screen_allowed_depths_iterator);
    XCB_FUNCTION(depth_next);
    XCB_FUNCTION(depth_visuals_iterator);
    XCB_FUNCTION(visualtype_next);
    XCB_FUNCTION(poll_for_reply);
    XCB_FUNCTION(poll_for_event);
    XCB_FUNCTION(change_window_attributes_checked);
    XCB_FUNCTION(send_event_checked);
    XCB_FUNCTION(flush);
    XCB_FUNCTION(request_check);
    XCB_FUNCTION(intern_atom);
    XCB_FUNCTION(get_property);
    XCB_FUNCTION(get_property_value);
    XCB_FUNCTION(get_property_value_length);
    XCB_FUNCTION(get_window_attributes);
    XCB_FUNCTION(get_geometry);
    XCB_FUNCTION(translate_coordinates);
    XCB_FUNCTION(query_tree);
    XCB_FUNCTION(query_tree_children);
    XCB_FUNCTION(query_tree_children_length);
    XCB_FUNCTION(get_input_focus);
    XCB_FUNCTION(set_input_focus_checked);
    XCB_FUNCTION(configure_window_checked);
    XCB_FUNCTION(query_pointer);
    XCB_FUNCTION(query_keymap);
    XCB_FUNCTION(get_keyboard_mapping);
    XCB_FUNCTION(get_keyboard_mapping_keysyms);
    XCB_FUNCTION(change_keyboard_mapping_checked);
    XCB_FUNCTION(get_image);
    XCB_FUNCTION(get_image_data);
    XCB_FUNCTION(get_image_data_length);
    XCB_FUNCTION(test_get_version);
    XCB_FUNCTION(test_fake_input_checked);
#undef XCB_FUNCTION
    template <typename T> void symbol(T& to, void* library, const char* name) {
        to = reinterpret_cast<T>(dlsym(library, name));
        if (!to)
            throw Error("COMPUTER_X11_DEPENDENCY: missing native XCB symbol");
    }
    Api() {
        core = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        test = dlopen("libxcb-xtest.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!core || !test) {
            if (test)
                dlclose(test);
            if (core)
                dlclose(core);
            throw Error("COMPUTER_X11_DEPENDENCY: install libxcb and its XTEST runtime library");
        }
        try {
#define LOAD(name) symbol(name, core, "xcb_" #name)
            LOAD(connect);
            LOAD(disconnect);
            LOAD(connection_has_error);
            LOAD(get_file_descriptor);
            LOAD(get_setup);
            LOAD(setup_roots_iterator);
            LOAD(screen_next);
            LOAD(setup_pixmap_formats_iterator);
            LOAD(format_next);
            LOAD(screen_allowed_depths_iterator);
            LOAD(depth_next);
            LOAD(depth_visuals_iterator);
            LOAD(visualtype_next);
            LOAD(poll_for_reply);
            LOAD(poll_for_event);
            LOAD(change_window_attributes_checked);
            LOAD(send_event_checked);
            LOAD(flush);
            LOAD(request_check);
            LOAD(intern_atom);
            LOAD(get_property);
            LOAD(get_property_value);
            LOAD(get_property_value_length);
            LOAD(get_window_attributes);
            LOAD(get_geometry);
            LOAD(translate_coordinates);
            LOAD(query_tree);
            LOAD(query_tree_children);
            LOAD(query_tree_children_length);
            LOAD(get_input_focus);
            LOAD(set_input_focus_checked);
            LOAD(configure_window_checked);
            LOAD(query_pointer);
            LOAD(query_keymap);
            LOAD(get_keyboard_mapping);
            LOAD(get_keyboard_mapping_keysyms);
            LOAD(change_keyboard_mapping_checked);
            LOAD(get_image);
            LOAD(get_image_data);
            LOAD(get_image_data_length);
#undef LOAD
            symbol(test_get_version, test, "xcb_test_get_version");
            symbol(test_fake_input_checked, test, "xcb_test_fake_input_checked");
        } catch (...) {
            dlclose(test);
            dlclose(core);
            throw;
        }
    }
    ~Api() {
        dlclose(test);
        dlclose(core);
    }
};
struct Free {
    void operator()(void* pointer) const {
        std::free(pointer);
    }
};
template <typename T> using Reply = std::unique_ptr<T, Free>;
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};
struct Window {
    xcb_window_t handle = 0;
    unsigned pid = 0;
    std::string id, title, start;
    Rect bounds;
};
class Display {
  public:
    Api& api;
    xcb_connection_t* connection = nullptr;
    xcb_screen_t* screen = nullptr;
    Cancel cancel;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(15);
    std::string name;
    explicit Display(const Cancel& token) : api(library()), cancel(token) {
        name = env_or("DISPLAY", "");
        if (name.size() < 2 || name.front() != ':' ||
            name.find_first_not_of("0123456789.", 1) != std::string::npos)
            throw Error("COMPUTER_X11_DISPLAY: a local DISPLAY such as :99 is required");
        const auto separator = name.find('.');
        auto decimal = [](std::string_view text) {
            unsigned value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value > 65535)
                throw Error("COMPUTER_X11_DISPLAY: invalid local display number");
            return value;
        };
        const auto display_number = decimal(
            std::string_view(name).substr(1, separator == name.npos ? name.size() - 1 : separator - 1));
        const auto screen_number =
            separator == name.npos ? 0u : decimal(std::string_view(name).substr(separator + 1));
        name = ":" + std::to_string(display_number) + "." + std::to_string(screen_number);
        int selected = 0;
        connection = api.connect(name.c_str(), &selected);
        try {
            health();
            auto roots = api.setup_roots_iterator(api.get_setup(connection));
            for (int i = 0; i < selected && roots.rem; ++i)
                api.screen_next(&roots);
            if (!roots.rem)
                throw Error("COMPUTER_DISPLAY_UNAVAILABLE");
            screen = roots.data;
            reply<xcb_test_get_version_reply_t>(api.test_get_version(connection, 2, 2));
        } catch (...) {
            api.disconnect(connection);
            connection = nullptr;
            throw;
        }
    }
    ~Display() {
        if (connection)
            api.disconnect(connection);
    }
    static Api& library() {
        static Api value;
        return value;
    }
    void health() {
        check(cancel);
        if (Clock::now() > deadline)
            throw Error("COMPUTER_ACTION_TIMEOUT");
        if (!connection || api.connection_has_error(connection))
            throw Error("COMPUTER_DESKTOP_UNAVAILABLE: cannot connect to the authorized local X11 desktop");
    }
    template <typename T, typename Cookie> Reply<T> reply(Cookie cookie) {
        api.flush(connection);
        for (;;) {
            health();
            void* result = nullptr;
            xcb_generic_error_t* error = nullptr;
            if (api.poll_for_reply(connection, cookie.sequence, &result, &error)) {
                Reply<xcb_generic_error_t> owned_error(error);
                Reply<T> owned(static_cast<T*>(result));
                if (error || !owned)
                    throw Error("COMPUTER_X11_REQUEST: window or display changed");
                return owned;
            }
            pollfd fd{api.get_file_descriptor(connection), POLLIN, 0};
            ::poll(&fd, 1, 10);
        }
    }
    void checked(xcb_void_cookie_t cookie) {
        // A bounded round trip first makes request_check nonblocking, including BadWindow.
        reply<xcb_get_input_focus_reply_t>(api.get_input_focus(connection));
        Reply<xcb_generic_error_t> error(api.request_check(connection, cookie));
        if (error)
            throw Error("COMPUTER_X11_REQUEST: input or window operation rejected");
    }
    xcb_atom_t atom(std::string_view value) {
        return reply<xcb_intern_atom_reply_t>(
                   api.intern_atom(connection, false, static_cast<std::uint16_t>(value.size()), value.data()))
            ->atom;
    }
    Reply<xcb_get_property_reply_t> property(xcb_window_t window, std::string_view key,
                                             xcb_atom_t type = XCB_GET_PROPERTY_TYPE_ANY) {
        return reply<xcb_get_property_reply_t>(
            api.get_property(connection, false, window, atom(key), type, 0, 4096));
    }
    std::vector<xcb_window_t> clients() {
        auto list = property(screen->root, "_NET_CLIENT_LIST_STACKING", XCB_ATOM_WINDOW);
        if (list->format == 32 && list->value_len <= 4096 && list->value_len) {
            const auto* values = static_cast<xcb_window_t*>(api.get_property_value(list.get()));
            return {values, values + list->value_len};
        }
        auto tree = reply<xcb_query_tree_reply_t>(api.query_tree(connection, screen->root));
        const auto* values = api.query_tree_children(tree.get());
        const int count = api.query_tree_children_length(tree.get());
        if (count < 0 || count > 4096)
            throw Error("COMPUTER_LAYOUT_UNSTABLE");
        return {values, values + count};
    }
    Window inspect(xcb_window_t handle) {
        auto attributes =
            reply<xcb_get_window_attributes_reply_t>(api.get_window_attributes(connection, handle));
        if (attributes->map_state != XCB_MAP_STATE_VIEWABLE ||
            attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT)
            throw Error("COMPUTER_WINDOW_UNAVAILABLE");
        auto owner = property(handle, "_NET_WM_PID", XCB_ATOM_CARDINAL);
        if (owner->format != 32 || owner->value_len != 1)
            throw Error("COMPUTER_PROCESS_UNAVAILABLE");
        Window value;
        value.handle = handle;
        std::memcpy(&value.pid, api.get_property_value(owner.get()), sizeof(value.pid));
        const auto process = fs::path("/proc") / std::to_string(value.pid);
        struct stat identity{};
        if (!value.pid || ::stat(process.c_str(), &identity) || identity.st_uid != geteuid())
            throw Error(
                "COMPUTER_SESSION_MISMATCH: target must belong to the same local user and PID namespace");
        std::string stat;
        try {
            stat = read_file(process / "stat", 8192);
        } catch (const std::system_error&) {
            throw Error("COMPUTER_PROCESS_UNAVAILABLE: process exited or became inaccessible");
        }
        const auto close = stat.rfind(')');
        if (close == stat.npos)
            throw Error("COMPUTER_PROCESS_UNAVAILABLE");
        std::istringstream fields(stat.substr(close + 2));
        for (int index = 3; index <= 22; ++index)
            if (!(fields >> value.start))
                throw Error("COMPUTER_PROCESS_UNAVAILABLE");
        auto title = property(handle, "_NET_WM_NAME");
        if (title->format != 8 || !title->value_len)
            title = property(handle, "WM_NAME");
        if (title->format == 8)
            value.title.assign(static_cast<const char*>(api.get_property_value(title.get())),
                               std::min<std::size_t>(title->value_len, 4096));
        auto geometry = reply<xcb_get_geometry_reply_t>(api.get_geometry(connection, handle));
        auto location = reply<xcb_translate_coordinates_reply_t>(
            api.translate_coordinates(connection, handle, screen->root, 0, 0));
        value.bounds = {location->dst_x, location->dst_y, geometry->width, geometry->height};
        if (value.bounds.width < 1 || value.bounds.height < 1 || value.bounds.x < 0 || value.bounds.y < 0 ||
            value.bounds.x + value.bounds.width > screen->width_in_pixels ||
            value.bounds.y + value.bounds.height > screen->height_in_pixels)
            throw Error("COMPUTER_WINDOW_UNAVAILABLE: move the entire window onto this display");
        value.id = name + ":" + std::to_string(handle) + ":" + std::to_string(value.pid) + ":" + value.start;
        return value;
    }
    Window resolve(const std::string& id) {
        for (const auto handle : clients()) {
            try {
                auto value = inspect(handle);
                if (value.id == id)
                    return value;
            } catch (const Error&) {
                health();
            }
        }
        throw Error("COMPUTER_WINDOW_CHANGED: list visible windows again");
    }
    bool belongs(xcb_window_t child, xcb_window_t target) {
        if (child == XCB_NONE || child == XCB_INPUT_FOCUS_POINTER_ROOT)
            return false;
        for (unsigned depth = 0; child && child != screen->root && depth < 64; ++depth) {
            if (child == target)
                return true;
            auto tree = reply<xcb_query_tree_reply_t>(api.query_tree(connection, child));
            child = tree->parent;
        }
        return false;
    }
    bool foreground(const Window& window) {
        return belongs(reply<xcb_get_input_focus_reply_t>(api.get_input_focus(connection))->focus,
                       window.handle);
    }
    void focus(const Window& window) {
        const std::uint32_t above = XCB_STACK_MODE_ABOVE;
        checked(
            api.configure_window_checked(connection, window.handle, XCB_CONFIG_WINDOW_STACK_MODE, &above));
        checked(
            api.set_input_focus_checked(connection, XCB_INPUT_FOCUS_PARENT, window.handle, XCB_CURRENT_TIME));
        delay(50, cancel);
        if (!foreground(window))
            throw Error("COMPUTER_FOCUS_UNAVAILABLE");
    }
    void current(const Window& expected, bool title = true) {
        health();
        auto live = inspect(expected.handle);
        if (live.id != expected.id || live.bounds != expected.bounds ||
            (title && live.title != expected.title) || !foreground(live))
            throw Error("COMPUTER_STALE_OBSERVATION: focus, title, process or bounds changed; observe again");
    }
    Json json(const Window& window) {
        return Json{{"window_id", window.id},         {"pid", window.pid},
                    {"title", window.title},          {"left", window.bounds.x},
                    {"top", window.bounds.y},         {"width", window.bounds.width},
                    {"height", window.bounds.height}, {"foreground", foreground(window)}};
    }
    void unoccluded(const Window& window) {
        // Check the actual topmost root child, including override-redirect popups.
        auto tree = reply<xcb_query_tree_reply_t>(api.query_tree(connection, screen->root));
        auto own = window.handle;
        for (unsigned depth = 0; depth < 64; ++depth) {
            auto parent = reply<xcb_query_tree_reply_t>(api.query_tree(connection, own));
            if (parent->parent == screen->root)
                break;
            own = parent->parent;
        }
        bool above = false;
        const auto* children = api.query_tree_children(tree.get());
        const auto count = api.query_tree_children_length(tree.get());
        if (count > 4096)
            throw Error("COMPUTER_LAYOUT_UNSTABLE");
        for (int i = 0; i < count; ++i) {
            if (children[i] == own) {
                above = true;
                continue;
            }
            if (!above)
                continue;
            auto attr =
                reply<xcb_get_window_attributes_reply_t>(api.get_window_attributes(connection, children[i]));
            if (attr->map_state != XCB_MAP_STATE_VIEWABLE || attr->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT)
                continue;
            // Non-activating application menus/tooltips are part of the target's
            // visible UI. An independent window or unidentified overlay remains
            // an occluder, even when it is in the same desktop session.
            if (attr->override_redirect) {
                auto owner = property(children[i], "_NET_WM_PID", XCB_ATOM_CARDINAL);
                std::uint32_t pid = 0;
                if (owner->format == 32 && owner->value_len == 1)
                    std::memcpy(&pid, api.get_property_value(owner.get()), sizeof(pid));
                auto transient = property(children[i], "WM_TRANSIENT_FOR", XCB_ATOM_WINDOW);
                std::uint32_t parent = 0;
                if (transient->format == 32 && transient->value_len == 1)
                    std::memcpy(&parent, api.get_property_value(transient.get()), sizeof(parent));
                if (pid == window.pid && (!parent || parent == window.handle))
                    continue;
            }
            auto g = reply<xcb_get_geometry_reply_t>(api.get_geometry(connection, children[i]));
            const auto& b = window.bounds;
            if (g->x < b.x + b.width && g->y < b.y + b.height && g->x + g->width > b.x &&
                g->y + g->height > b.y)
                throw Error("COMPUTER_WINDOW_OCCLUDED: another window covers the target; observe again");
        }
        if (!above)
            throw Error("COMPUTER_WINDOW_UNAVAILABLE");
    }
    void input(std::uint8_t type, std::uint8_t detail, int x = 0, int y = 0) {
        checked(api.test_fake_input_checked(connection, type, detail, XCB_CURRENT_TIME, screen->root,
                                            static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), 0));
    }
    template <typename Function> void cleanup(Function function) noexcept {
        auto previous_cancel = std::move(cancel);
        const auto previous_deadline = deadline;
        deadline = Clock::now() + std::chrono::seconds(1);
        try {
            function();
        } catch (...) {
        }
        cancel = std::move(previous_cancel);
        deadline = previous_deadline;
    }
    void release(std::uint8_t type, std::uint8_t detail) noexcept {
        // Flush alone can lose XTEST releases when the connection immediately
        // closes. A bounded round trip must finish even after cancellation.
        cleanup([&] { input(type, detail); });
    }
    bool supports_ping(const Window& window) {
        auto protocols = property(window.handle, "WM_PROTOCOLS", XCB_ATOM_ATOM);
        if (protocols->format != 32 || !protocols->value_len)
            return false;
        const auto* values = static_cast<xcb_atom_t*>(api.get_property_value(protocols.get()));
        const auto ping = atom("_NET_WM_PING");
        return std::find(values, values + protocols->value_len, ping) != values + protocols->value_len;
    }
    void application_sync(const Window& window) {
        const std::uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        checked(api.change_window_attributes_checked(connection, screen->root, XCB_CW_EVENT_MASK, &mask));
        static std::atomic_uint32_t serial{1};
        xcb_client_message_event_t message{};
        message.response_type = XCB_CLIENT_MESSAGE;
        message.format = 32;
        message.window = window.handle;
        message.type = atom("WM_PROTOCOLS");
        message.data.data32[0] = atom("_NET_WM_PING");
        message.data.data32[1] = serial.fetch_add(1);
        message.data.data32[2] = window.handle;
        checked(api.send_event_checked(connection, false, window.handle, XCB_EVENT_MASK_NO_EVENT,
                                       reinterpret_cast<const char*>(&message)));
        const auto until = std::min(deadline, Clock::now() + std::chrono::seconds(2));
        while (Clock::now() < until) {
            health();
            while (auto* raw = api.poll_for_event(connection)) {
                Reply<xcb_generic_event_t> event(raw);
                health();
                if ((event->response_type & 0x7f) != XCB_CLIENT_MESSAGE)
                    continue;
                const auto* reply = reinterpret_cast<xcb_client_message_event_t*>(event.get());
                if (reply->window == screen->root && reply->type == message.type && reply->format == 32 &&
                    std::equal(std::begin(message.data.data32), std::begin(message.data.data32) + 3,
                               std::begin(reply->data.data32)))
                    return;
            }
            delay(5, cancel);
        }
        throw Error("COMPUTER_APPLICATION_TIMEOUT: the target did not acknowledge its X11 event queue");
    }
};
class InputLease {
    NativeHandle file_;

  public:
    explicit InputLease(const std::string& display) {
        // All screens and spelling aliases share the X server's input devices.
        const auto path = "/tmp/devbox-native-x11-" + std::to_string(geteuid()) + "-" +
                          display.substr(0, display.find('.')) + ".lock";
        file_.reset(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        struct stat state{};
        if (!file_ || fstat(file_.get(), &state) || !S_ISREG(state.st_mode) || state.st_uid != geteuid() ||
            state.st_nlink != 1 || (state.st_mode & 077) || flock(file_.get(), LOCK_EX | LOCK_NB))
            throw Error("COMPUTER_BUSY: cannot acquire the local display input lease");
    }
};
std::uint32_t keysym(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const std::map<std::string, std::uint32_t> names{
        {"CTRL", 0xffe3},   {"CONTROL", 0xffe3},   {"SHIFT", 0xffe1},  {"ALT", 0xffe9},
        {"ENTER", 0xff0d},  {"RETURN", 0xff0d},    {"TAB", 0xff09},    {"ESC", 0xff1b},
        {"ESCAPE", 0xff1b}, {"BACKSPACE", 0xff08}, {"DELETE", 0xffff}, {"INSERT", 0xff63},
        {"HOME", 0xff50},   {"END", 0xff57},       {"PAGEUP", 0xff55}, {"PAGEDOWN", 0xff56},
        {"LEFT", 0xff51},   {"UP", 0xff52},        {"RIGHT", 0xff53},  {"DOWN", 0xff54},
        {"SPACE", 32}};
    if (const auto found = names.find(value); found != names.end())
        return found->second;
    if (value.size() == 1 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= '0' && value[0] <= '9')))
        return value[0] >= 'A' ? static_cast<unsigned>(value[0] + 32) : static_cast<unsigned>(value[0]);
    if (value.size() >= 2 && value[0] == 'F') {
        unsigned number = 0;
        auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), number);
        if (error == std::errc{} && end == value.data() + value.size() && number >= 1 && number <= 12)
            return 0xffbd + number;
    }
    throw Error("COMPUTER_KEY_INVALID: use a supported named key");
}
struct Keyboard {
    Display& display;
    std::uint8_t first, count, per;
    std::vector<xcb_keysym_t> symbols;
    explicit Keyboard(Display& d) : display(d) {
        const auto* setup = d.api.get_setup(d.connection);
        first = setup->min_keycode;
        count = static_cast<std::uint8_t>(setup->max_keycode - first + 1);
        auto map =
            d.reply<xcb_get_keyboard_mapping_reply_t>(d.api.get_keyboard_mapping(d.connection, first, count));
        per = map->keysyms_per_keycode;
        if (!per)
            throw Error("COMPUTER_KEYMAP_UNAVAILABLE");
        const auto* values = d.api.get_keyboard_mapping_keysyms(map.get());
        symbols.assign(values, values + static_cast<std::size_t>(per) * count);
    }
    std::uint8_t code(std::uint32_t symbol) {
        for (unsigned i = 0; i < count; ++i)
            if (symbols[i * per] == symbol)
                return static_cast<std::uint8_t>(first + i);
        throw Error("COMPUTER_KEY_UNAVAILABLE: current X11 layout lacks the requested key");
    }
    std::pair<std::uint8_t, bool> literal(std::uint32_t symbol) const {
        for (unsigned i = 0; i < count; ++i) {
            if (symbols[i * per] == symbol)
                return {static_cast<std::uint8_t>(first + i), false};
            if (per > 1 && symbols[i * per + 1] == symbol)
                return {static_cast<std::uint8_t>(first + i), true};
        }
        return {0, false};
    }
    std::vector<std::uint8_t> chord(const Json& keys, bool empty = false) {
        if (!keys.is_array() || (!empty && keys.empty()) || keys.size() > 8)
            throw Error("COMPUTER_KEYS_INVALID");
        std::vector<std::uint8_t> codes;
        std::vector<std::uint32_t> syms;
        for (const auto& key : keys) {
            if (!key.is_string())
                throw Error("COMPUTER_KEYS_INVALID");
            auto symbol = keysym(key.get<std::string>());
            if (std::find(syms.begin(), syms.end(), symbol) != syms.end())
                throw Error("COMPUTER_DUPLICATE_KEY");
            syms.push_back(symbol);
            codes.push_back(code(symbol));
        }
        auto has = [&](std::uint32_t s) { return std::find(syms.begin(), syms.end(), s) != syms.end(); };
        if ((has(0xffe9) && (has(0xff09) || has(0xff1b) || has(0xffbe))) ||
            (has(0xffe3) &&
             (has(0xff1b) || (has(0xffe9) && (has(0xffff) || has(0xff08) ||
                                              std::any_of(syms.begin(), syms.end(), [](auto s) {
                                                  return s >= 0xffbe && s <= 0xffc9;
                                              }))))))
            throw Error("COMPUTER_SYSTEM_SHORTCUT_DENIED");
        return codes;
    }
    void unheld() {
        auto state = display.reply<xcb_query_keymap_reply_t>(display.api.query_keymap(display.connection));
        // Refuse all held keys: do not inherit physical input or another client's modifiers.
        for (unsigned char byte : state->keys)
            if (byte)
                throw Error("COMPUTER_INPUT_BUSY: release held keys");
        auto pointer = display.reply<xcb_query_pointer_reply_t>(
            display.api.query_pointer(display.connection, display.screen->root));
        if (pointer->mask & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3 | XCB_MOD_MASK_LOCK |
                             XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4))
            throw Error("COMPUTER_INPUT_BUSY: release held buttons and modifiers");
    }
};
void big_endian(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift : {24, 16, 8, 0})
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void chunk(std::vector<std::uint8_t>& png, const char* type, std::span<const std::uint8_t> data) {
    big_endian(png, static_cast<std::uint32_t>(data.size()));
    const auto start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    big_endian(png,
               static_cast<std::uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(data.size() + 4))));
}
ImageCapture capture(Display& d, const Window& window, unsigned max_width) {
    const auto& b = window.bounds;
    if (static_cast<std::uint64_t>(b.width) * b.height > 16777216)
        throw Error("COMPUTER_IMAGE_BOUNDS");
    auto pixels = d.reply<xcb_get_image_reply_t>(d.api.get_image(
        d.connection, XCB_IMAGE_FORMAT_Z_PIXMAP, d.screen->root, static_cast<std::int16_t>(b.x),
        static_cast<std::int16_t>(b.y), static_cast<std::uint16_t>(b.width),
        static_cast<std::uint16_t>(b.height), UINT32_MAX));
    const auto* setup = d.api.get_setup(d.connection);
    unsigned bits = 0, pad = 0;
    for (auto formats = d.api.setup_pixmap_formats_iterator(setup); formats.rem; d.api.format_next(&formats))
        if (formats.data->depth == pixels->depth) {
            bits = formats.data->bits_per_pixel;
            pad = formats.data->scanline_pad;
        }
    if ((bits != 32 && bits != 24) || pad != 32 || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST)
        throw Error("COMPUTER_PIXEL_FORMAT: use a 24-bit TrueColor X11 desktop");
    std::uint32_t red = 0, green = 0, blue = 0;
    for (auto depths = d.api.screen_allowed_depths_iterator(d.screen); depths.rem; d.api.depth_next(&depths))
        for (auto visuals = d.api.depth_visuals_iterator(depths.data); visuals.rem;
             d.api.visualtype_next(&visuals))
            if (visuals.data->visual_id == d.screen->root_visual) {
                red = visuals.data->red_mask;
                green = visuals.data->green_mask;
                blue = visuals.data->blue_mask;
            }
    if (red != 0xff0000 || green != 0xff00 || blue != 0xff)
        throw Error("COMPUTER_PIXEL_FORMAT");
    const auto stride = (static_cast<unsigned>(b.width) * bits + 31) / 32 * 4;
    if (static_cast<std::uint64_t>(stride) * b.height >
        static_cast<unsigned>(d.api.get_image_data_length(pixels.get())))
        throw Error("COMPUTER_IMAGE_TRUNCATED");
    const unsigned width = std::min(max_width, static_cast<unsigned>(b.width));
    const unsigned height =
        std::max(1u, static_cast<unsigned>(static_cast<std::uint64_t>(b.height) * width / b.width));
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(width * 3 + 1) * height);
    const auto* source = d.api.get_image_data(pixels.get());
    for (unsigned y = 0; y < height; ++y) {
        d.health();
        const auto* row = source + (static_cast<std::uint64_t>(y) * b.height / height) * stride;
        auto* out = raw.data() + static_cast<std::size_t>(y) * (width * 3 + 1);
        *out++ = 0;
        for (unsigned x = 0; x < width; ++x) {
            const auto* pixel = row + (static_cast<std::uint64_t>(x) * b.width / width) * (bits / 8);
            *out++ = pixel[2];
            *out++ = pixel[1];
            *out++ = pixel[0];
        }
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(size);
    if (compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK)
        throw Error("COMPUTER_IMAGE_ENCODING");
    compressed.resize(size);
    std::vector<std::uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10}, header;
    big_endian(header, width);
    big_endian(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", compressed);
    chunk(png, "IEND", {});
    validate_capture_image(png, "image/png");
    return {std::move(png), "image/png",
            Json{{"image_width", width}, {"image_height", height}, {"capture_backend", "native-xcb"}}};
}
} // namespace

bool computer_x11_enabled() {
    return env_or("DEVBOX_COMPUTER_USE_X11", "") == "1" && !env_or("DISPLAY", "").empty();
}
struct ComputerX11::Impl {
    std::mutex operation;
    struct Observation {
        Window window;
        Clock::time_point created;
        unsigned width = 0, height = 0, max_width = 1600;
    };
    std::map<std::string, Observation> observations;
};
ComputerX11::ComputerX11() : impl_(std::make_unique<Impl>()) {}
ComputerX11::~ComputerX11() = default;
Json ComputerX11::windows(const Json& args, const Cancel& cancel) {
    std::unique_lock lock(impl_->operation, std::try_to_lock);
    if (!lock.owns_lock())
        throw Error("COMPUTER_BUSY");
    Display display(cancel);
    Json windows = Json::array();
    Json skipped = Json::object();
    bool truncated = false;
    const auto filter = lower(json_string(args, "title_contains"));
    for (const auto handle : display.clients()) {
        try {
            const auto window = display.inspect(handle);
            if (window.title.empty() ||
                (!filter.empty() && lower(window.title).find(filter) == std::string::npos))
                continue;
            if (windows.size() == 128) {
                truncated = true;
                break;
            }
            windows.push_back(display.json(window));
        } catch (const Error& error) {
            const std::string reason(error.what());
            const auto code = reason.substr(0, reason.find(':'));
            skipped[code] = skipped.value(code, 0u) + 1;
            display.health();
        }
    }
    return Json{{"supported", true},
                {"platform", "linux-x11"},
                {"usage_type", "computer_use"},
                {"windows", windows},
                {"skipped", skipped},
                {"truncated", truncated},
                {"next", "Call host_computer_use action observe with a returned window_id."}};
}
ImageCapture ComputerX11::perform(const Json& args, const Cancel& cancel) {
    std::unique_lock lock(impl_->operation, std::try_to_lock);
    if (!lock.owns_lock())
        throw Error("COMPUTER_BUSY");
    Display d(cancel);
    InputLease lease(d.name);
    Keyboard keyboard(d);
    keyboard.unheld();
    const auto action = json_string(args, "action"), old_id = json_string(args, "observation_id");
    for (auto it = impl_->observations.begin(); it != impl_->observations.end();)
        if (Clock::now() - it->second.created > std::chrono::seconds(180))
            it = impl_->observations.erase(it);
        else
            ++it;
    Impl::Observation o{};
    if (action == "observe") {
        o.window = d.resolve(json_string(args, "window_id"));
        o.max_width = static_cast<unsigned>(json_uint(args, "max_width", 1600));
        const auto quality = json_uint(args, "quality", 70);
        if (o.max_width < 640 || o.max_width > 1920 || quality < 30 || quality > 90)
            throw Error("COMPUTER_IMAGE_BOUNDS");
        d.focus(o.window);
        o.window = d.inspect(o.window.handle);
    } else {
        const auto found = impl_->observations.find(old_id);
        if (found == impl_->observations.end())
            throw Error("COMPUTER_STALE_OBSERVATION: observe first; IDs expire and are single-use");
        o = found->second;
        const auto requested = json_string(args, "window_id");
        if (!requested.empty() && requested != o.window.id)
            throw Error("COMPUTER_WINDOW_MISMATCH");
        d.current(o.window);
    }
    d.unoccluded(o.window);
    using Point = std::pair<int, int>;
    auto point = [&](const Json& value) -> Point {
        if (!value.contains("x") || !value.contains("y") || !value["x"].is_number_integer() ||
            !value["y"].is_number_integer())
            throw Error("COMPUTER_COORDINATES_REQUIRED");
        const auto x = value["x"].get<std::int64_t>(), y = value["y"].get<std::int64_t>();
        if (x < 0 || y < 0 || x >= o.width || y >= o.height)
            throw Error("COMPUTER_COORDINATES_OUTSIDE_IMAGE");
        return {o.window.bounds.x + static_cast<int>(x * o.window.bounds.width / o.width),
                o.window.bounds.y + static_cast<int>(y * o.window.bounds.height / o.height)};
    };
    const auto duration = json_uint(args, "duration_ms", 300), hold = json_uint(args, "hold_ms", 0),
               settle = json_uint(args, "settle_ms", 100);
    if (duration > 5000 || hold > 5000 || settle > 1000)
        throw Error("COMPUTER_DURATION_OUT_OF_RANGE");
    std::vector<Point> points;
    if (action == "click" || action == "double_click" || action == "move" || action == "scroll")
        points.push_back(point(args));
    if (action == "drag") {
        const auto path = args.value("path", Json::array());
        if (!path.is_array() || path.size() < 2 || path.size() > 256)
            throw Error("COMPUTER_DRAG_PATH_INVALID");
        for (const auto& value : path)
            points.push_back(point(value));
    }
    const auto button = json_string(args, "button", "left");
    const std::uint8_t mouse = button == "left" ? 1 : button == "middle" ? 2 : 3;
    if (button != "left" && button != "middle" && button != "right")
        throw Error("COMPUTER_BUTTON_INVALID");
    std::vector<std::pair<std::vector<std::uint8_t>, unsigned>> segments;
    if (action == "key")
        segments.push_back({keyboard.chord(args.value("keys", Json::array())), static_cast<unsigned>(hold)});
    std::uint64_t sequence_duration = 0;
    if (action == "key_sequence") {
        const auto entries = args.value("sequence", Json::array());
        if (!entries.is_array() || entries.empty() || entries.size() > 32)
            throw Error("COMPUTER_SEQUENCE_INVALID");
        for (const auto& entry : entries) {
            if (!entry.is_object() || entry.size() != 2 || !entry.contains("keys") ||
                !entry.contains("duration_ms") || !entry["duration_ms"].is_number_integer())
                throw Error("COMPUTER_SEQUENCE_INVALID");
            const auto ms = entry["duration_ms"].get<std::int64_t>();
            if (ms < 1 || ms > 5000 || (sequence_duration += static_cast<unsigned>(ms)) > 5000)
                throw Error("COMPUTER_SEQUENCE_DURATION");
            segments.push_back({keyboard.chord(entry["keys"], true), static_cast<unsigned>(ms)});
        }
    }
    std::vector<std::uint32_t> typed;
    if (action == "type") {
        const auto text = to_utf16(json_string(args, "text"));
        if (text.empty() || text.size() > 8192)
            throw Error("COMPUTER_TEXT_INVALID");
        for (std::size_t i = 0; i < text.size(); ++i) {
            std::uint32_t code = text[i];
            if (code >= 0xd800 && code <= 0xdbff && i + 1 < text.size()) {
                const auto low = text[++i];
                if (low < 0xdc00 || low > 0xdfff)
                    throw Error("COMPUTER_TEXT_INVALID");
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            }
            if (code < 32 || code == 127 || (code >= 0xd800 && code <= 0xdfff))
                throw Error("COMPUTER_TEXT_INVALID");
            typed.push_back(code <= 255 ? code : 0x01000000 | code);
        }
    }
    const auto sx = args.value("scroll_x", std::int64_t(0)), sy = args.value("scroll_y", std::int64_t(0));
    if (sx < -20 || sx > 20 || sy < -20 || sy > 20 || (action == "scroll" && !sx && !sy))
        throw Error("COMPUTER_SCROLL_INVALID");
    unsigned spare = 0;
    const auto unmapped = std::count_if(typed.begin(), typed.end(),
                                        [&](auto symbol) { return !keyboard.literal(symbol).first; });
    if (typed.size() + static_cast<std::size_t>(unmapped) * 10 > 1024)
        throw Error("COMPUTER_TEXT_BUDGET: split text into smaller calls before typing (1024 work units; "
                    "mapped characters cost 1, unmapped Unicode characters cost 11). No input was sent.");
    if (unmapped) {
        if (!d.supports_ping(o.window))
            throw Error("COMPUTER_TEXT_LAYOUT_UNAVAILABLE: this app cannot acknowledge temporary Unicode "
                        "key mappings; use characters in its keyboard layout. No input was sent.");
        for (unsigned i = 0; i < keyboard.count; ++i)
            if (std::all_of(keyboard.symbols.begin() + i * keyboard.per,
                            keyboard.symbols.begin() + (i + 1) * keyboard.per,
                            [](auto s) { return s == 0; })) {
                spare = keyboard.first + i;
                break;
            }
        if (!spare)
            throw Error("COMPUTER_KEYMAP_UNAVAILABLE: no unused keycode for literal Unicode input");
    }
    if (action != "observe")
        impl_->observations.erase(old_id);
    std::erase_if(impl_->observations,
                  [&](const auto& entry) { return entry.second.window.id == o.window.id; });
    bool attempted = false;
    unsigned scroll_events = 0;
    auto send = [&](std::uint8_t type, std::uint8_t detail, int x = 0, int y = 0) {
        attempted = true;
        d.input(type, detail, x, y);
    };
    auto current = [&](bool title = false) {
        d.current(o.window, title);
        d.unoccluded(o.window);
    };
    auto move = [&](Point p) {
        current();
        send(XCB_MOTION_NOTIFY, 0, p.first, p.second);
    };
    auto tap = [&](std::uint8_t code) {
        ScopeExit release([&] { d.release(XCB_BUTTON_RELEASE, code); });
        send(XCB_BUTTON_PRESS, code);
        delay(15, cancel);
        send(XCB_BUTTON_RELEASE, code);
        release.disarm();
    };
    try {
        if (action == "move")
            move(points.front());
        if (action == "click" || action == "double_click") {
            move(points.front());
            tap(mouse);
            if (action == "double_click") {
                delay(60, cancel);
                current();
                tap(mouse);
            }
        }
        if (action == "drag") {
            move(points.front());
            ScopeExit release([&] { d.release(XCB_BUTTON_RELEASE, mouse); });
            send(XCB_BUTTON_PRESS, mouse);
            const auto start = Clock::now();
            for (std::size_t i = 1; i < points.size(); ++i) {
                const auto when = start + Millis(duration * i / (points.size() - 1));
                while (Clock::now() < when) {
                    current();
                    delay(10, cancel);
                }
                move(points[i]);
            }
            send(XCB_BUTTON_RELEASE, mouse);
            release.disarm();
        }
        if (action == "scroll") {
            move(points.front());
            for (const auto& [delta, positive, negative] : {std::tuple{sy, 5, 4}, std::tuple{sx, 7, 6}})
                for (std::int64_t i = 0; i < std::abs(delta); ++i) {
                    current();
                    tap(static_cast<std::uint8_t>(delta > 0 ? positive : negative));
                    ++scroll_events;
                }
        }
        if (!segments.empty()) {
            std::vector<std::uint8_t> held;
            ScopeExit release([&] {
                for (auto it = held.rbegin(); it != held.rend(); ++it)
                    d.release(XCB_KEY_RELEASE, *it);
            });
            for (const auto& [keys, ms] : segments) {
                current(action == "key_sequence");
                for (std::size_t i = held.size(); i > 0; --i)
                    if (std::find(keys.begin(), keys.end(), held[i - 1]) == keys.end()) {
                        send(XCB_KEY_RELEASE, held[i - 1]);
                        held.erase(held.begin() + static_cast<std::ptrdiff_t>(i - 1));
                    }
                for (const auto key : keys)
                    if (std::find(held.begin(), held.end(), key) == held.end()) {
                        held.push_back(key);
                        send(XCB_KEY_PRESS, key);
                    }
                const auto until = Clock::now() + Millis(ms);
                do {
                    current(action == "key_sequence");
                    delay(10, cancel);
                } while (Clock::now() < until);
            }
            for (auto it = held.rbegin(); it != held.rend(); ++it)
                send(XCB_KEY_RELEASE, *it);
            held.clear();
        }
        if (action == "type") {
            const auto offset = spare ? (spare - keyboard.first) * keyboard.per : 0;
            ScopeExit restore([&] {
                if (spare) {
                    d.release(XCB_KEY_RELEASE, static_cast<std::uint8_t>(spare));
                    d.cleanup([&] {
                        d.checked(d.api.change_keyboard_mapping_checked(
                            d.connection, 1, static_cast<std::uint8_t>(spare), keyboard.per,
                            keyboard.symbols.data() + offset));
                    });
                }
            });
            for (const auto symbol : typed) {
                current();
                const auto [mapped, shifted] = keyboard.literal(symbol);
                if (mapped) {
                    const auto shift = shifted ? keyboard.code(0xffe1) : 0;
                    ScopeExit release([&] {
                        d.release(XCB_KEY_RELEASE, mapped);
                        if (shift)
                            d.release(XCB_KEY_RELEASE, static_cast<std::uint8_t>(shift));
                    });
                    if (shift)
                        send(XCB_KEY_PRESS, static_cast<std::uint8_t>(shift));
                    send(XCB_KEY_PRESS, mapped);
                    send(XCB_KEY_RELEASE, mapped);
                    if (shift)
                        send(XCB_KEY_RELEASE, static_cast<std::uint8_t>(shift));
                    release.disarm();
                    continue;
                }
                const std::array<xcb_keysym_t, 2> symbols{symbol, symbol};
                d.checked(d.api.change_keyboard_mapping_checked(
                    d.connection, 1, static_cast<std::uint8_t>(spare), 2, symbols.data()));
                d.application_sync(o.window);
                // The acknowledgement can take time; revalidate after the wait
                // so a popup or another application cannot receive this glyph.
                current();
                send(XCB_KEY_PRESS, static_cast<std::uint8_t>(spare));
                send(XCB_KEY_RELEASE, static_cast<std::uint8_t>(spare));
                // Server synchronization does not mean the app has consumed
                // MappingNotify/KeyPress. Ack the app queue before reusing or
                // restoring the keycode, otherwise a busy Chromium loses text.
                d.application_sync(o.window);
            }
            if (spare)
                d.checked(
                    d.api.change_keyboard_mapping_checked(d.connection, 1, static_cast<std::uint8_t>(spare),
                                                          keyboard.per, keyboard.symbols.data() + offset));
            restore.disarm();
        }
        if (action == "wait")
            delay(static_cast<unsigned>(duration), cancel);
        delay(static_cast<unsigned>(settle), cancel);
        auto latest = d.inspect(o.window.handle);
        if (latest.id != o.window.id || !d.foreground(latest))
            throw Error("COMPUTER_FOCUS_CHANGED");
        d.unoccluded(latest);
        auto image = capture(d, latest, o.max_width);
        d.current(latest);
        d.unoccluded(latest);
        o.window = latest;
        o.created = Clock::now();
        o.width = static_cast<unsigned>(json_uint(image.metadata, "image_width"));
        o.height = static_cast<unsigned>(json_uint(image.metadata, "image_height"));
        while (impl_->observations.size() >= 32)
            impl_->observations.erase(std::min_element(
                impl_->observations.begin(), impl_->observations.end(),
                [](const auto& a, const auto& b) { return a.second.created < b.second.created; }));
        const auto id = uuid();
        impl_->observations.emplace(id, o);
        image.metadata.update(Json{{"platform", "linux-x11"},
                                   {"usage_type", "computer_use"},
                                   {"action", action},
                                   {"input_events_sent", attempted},
                                   {"window", d.json(latest)},
                                   {"observation_id", id},
                                   {"expires_after_seconds", 180},
                                   {"coordinate_space", "returned_image_pixels"},
                                   {"instruction", "Inspect this image. Use the new observation_id once. "
                                                   "Observe again after any error or window change."}});
        if (action == "scroll")
            image.metadata["scroll"] = Json{
                {"unit", "wheel_notches"},
                {"x", args.at("x")},
                {"y", args.at("y")},
                {"requested_x", sx},
                {"requested_y", sy},
                {"wheel_events_sent", scroll_events},
                {"content_movement_verified", false},
                {"instruction", "Inspect the returned image to confirm the intended pane moved. Scroll "
                                "inside its content, away from fixed headers, footers and number inputs."}};
        if (action == "key_sequence") {
            image.metadata["sequence_segments"] = segments.size();
            image.metadata["scheduled_duration_ms"] = sequence_duration;
        }
        return image;
    } catch (const std::exception& error) {
        if (attempted)
            throw Error("COMPUTER_INPUT_OUTCOME_UNKNOWN: input may have occurred; observation consumed. "
                        "Observe again before deciding to retry. " +
                        std::string(error.what()));
        throw;
    }
}
} // namespace devbox
#else
namespace devbox {
bool computer_x11_enabled() {
    return false;
}
struct ComputerX11::Impl {};
ComputerX11::ComputerX11() : impl_(std::make_unique<Impl>()) {}
ComputerX11::~ComputerX11() = default;
Json ComputerX11::windows(const Json&, const Cancel&) {
    throw Error("COMPUTER_UNSUPPORTED");
}
ImageCapture ComputerX11::perform(const Json&, const Cancel&) {
    throw Error("COMPUTER_UNSUPPORTED");
}
} // namespace devbox
#endif
