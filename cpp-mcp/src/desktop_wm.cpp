#include "devbox/desktop_wm.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#endif

namespace devbox {
#if defined(__linux__) && !defined(__ANDROID__)
namespace {
using Steady = std::chrono::steady_clock;
std::int64_t ticks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now().time_since_epoch()).count();
}
struct Free {
    void operator()(void* value) const {
        std::free(value);
    }
};
template <typename T> using Reply = std::unique_ptr<T, Free>;
struct Api {
    void* library = nullptr;
#define WM_FUNCTION(name) decltype(&::xcb_##name) name = nullptr
    WM_FUNCTION(connect_to_fd);
    WM_FUNCTION(disconnect);
    WM_FUNCTION(connection_has_error);
    WM_FUNCTION(get_setup);
    WM_FUNCTION(setup_roots_iterator);
    WM_FUNCTION(get_file_descriptor);
    WM_FUNCTION(flush);
    WM_FUNCTION(poll_for_reply);
    WM_FUNCTION(poll_for_event);
    WM_FUNCTION(request_check);
    WM_FUNCTION(intern_atom);
    WM_FUNCTION(generate_id);
    WM_FUNCTION(create_window);
    WM_FUNCTION(change_window_attributes_checked);
    WM_FUNCTION(change_window_attributes);
    WM_FUNCTION(change_property);
    WM_FUNCTION(delete_property);
    WM_FUNCTION(get_property);
    WM_FUNCTION(get_property_value);
    WM_FUNCTION(get_property_value_length);
    WM_FUNCTION(get_selection_owner);
    WM_FUNCTION(set_selection_owner);
    WM_FUNCTION(query_tree);
    WM_FUNCTION(query_tree_children);
    WM_FUNCTION(query_tree_children_length);
    WM_FUNCTION(get_window_attributes);
    WM_FUNCTION(get_geometry);
    WM_FUNCTION(configure_window);
    WM_FUNCTION(map_window);
    WM_FUNCTION(unmap_window);
    WM_FUNCTION(send_event);
    WM_FUNCTION(get_input_focus);
    WM_FUNCTION(set_input_focus);
    WM_FUNCTION(grab_button);
    WM_FUNCTION(allow_events);
#undef WM_FUNCTION
    Api() {
        library = ::dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            throw std::runtime_error("DESKTOP_XCB_UNAVAILABLE");
        try {
#define LOAD(name)                                                                                           \
    name = reinterpret_cast<decltype(name)>(::dlsym(library, "xcb_" #name));                                 \
    if (!name)                                                                                               \
    throw std::runtime_error("DESKTOP_XCB_SYMBOL_UNAVAILABLE")
            LOAD(connect_to_fd);
            LOAD(disconnect);
            LOAD(connection_has_error);
            LOAD(get_setup);
            LOAD(setup_roots_iterator);
            LOAD(get_file_descriptor);
            LOAD(flush);
            LOAD(poll_for_reply);
            LOAD(poll_for_event);
            LOAD(request_check);
            LOAD(intern_atom);
            LOAD(generate_id);
            LOAD(create_window);
            LOAD(change_window_attributes_checked);
            LOAD(change_window_attributes);
            LOAD(change_property);
            LOAD(delete_property);
            LOAD(get_property);
            LOAD(get_property_value);
            LOAD(get_property_value_length);
            LOAD(get_selection_owner);
            LOAD(set_selection_owner);
            LOAD(query_tree);
            LOAD(query_tree_children);
            LOAD(query_tree_children_length);
            LOAD(get_window_attributes);
            LOAD(get_geometry);
            LOAD(configure_window);
            LOAD(map_window);
            LOAD(unmap_window);
            LOAD(send_event);
            LOAD(get_input_focus);
            LOAD(set_input_focus);
            LOAD(grab_button);
            LOAD(allow_events);
#undef LOAD
        } catch (...) {
            ::dlclose(library);
            throw;
        }
    }
    ~Api() {
        ::dlclose(library);
    }
};
struct Gone : std::runtime_error {
    Gone() : std::runtime_error("DESKTOP_WINDOW_CHANGED") {}
};
struct Geometry {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 1, height = 1;
};
struct Client {
    Geometry normal;
    bool fullscreen = false, horizontal = false, vertical = false, minimized = false;
};
class Manager {
    struct Socket {
        int value;
        ~Socket() {
            if (value >= 0)
                ::close(value);
        }
    } socket_;
    Api api_;
    xcb_connection_t* connection_ = nullptr;
    xcb_screen_t* screen_ = nullptr;
    xcb_window_t support_ = 0, active_ = 0;
    std::map<std::string, xcb_atom_t, std::less<>> atoms_;
    std::map<xcb_window_t, Client> clients_;
    std::vector<xcb_window_t> order_;
    std::atomic<bool>& stop_;
    std::atomic<std::int64_t>& heartbeat_;
    xcb_atom_t atom(std::string_view name) {
        if (const auto entry = atoms_.find(name); entry != atoms_.end())
            return entry->second;
        const auto value =
            reply<xcb_intern_atom_reply_t>(
                api_.intern_atom(connection_, false, static_cast<std::uint16_t>(name.size()), name.data()))
                ->atom;
        atoms_.emplace(name, value);
        return value;
    }
    void check() {
        if (stop_.load() || !connection_ || api_.connection_has_error(connection_))
            throw std::runtime_error("DESKTOP_WINDOW_MANAGER_DISCONNECTED");
    }
    template <typename T, typename Cookie> Reply<T> reply(Cookie cookie) {
        api_.flush(connection_);
        const auto until = Steady::now() + std::chrono::seconds(2);
        for (;;) {
            check();
            void* value = nullptr;
            xcb_generic_error_t* error = nullptr;
            if (api_.poll_for_reply(connection_, cookie.sequence, &value, &error)) {
                Reply<xcb_generic_error_t> owned_error(error);
                Reply<T> owned(static_cast<T*>(value));
                if (error || !owned)
                    throw Gone();
                return owned;
            }
            if (Steady::now() >= until)
                throw std::runtime_error("DESKTOP_WINDOW_MANAGER_REPLY_DEADLINE");
            pollfd descriptor{api_.get_file_descriptor(connection_), POLLIN, 0};
            ::poll(&descriptor, 1, 20);
        }
    }
    std::vector<std::uint32_t> property(xcb_window_t window, xcb_atom_t name, xcb_atom_t type,
                                        unsigned limit = 64) {
        const auto value = reply<xcb_get_property_reply_t>(
            api_.get_property(connection_, false, window, name, type, 0, limit));
        if (value->format != 32 || !value->value_len || value->value_len > limit || value->bytes_after)
            return {};
        const auto* data = static_cast<const std::uint32_t*>(api_.get_property_value(value.get()));
        return {data, data + value->value_len};
    }
    void set(xcb_window_t window, std::string_view name, xcb_atom_t type,
             std::span<const std::uint32_t> data) {
        api_.change_property(connection_, XCB_PROP_MODE_REPLACE, window, atom(name), type, 32,
                             static_cast<std::uint32_t>(data.size()), data.data());
    }
    void set(xcb_window_t window, std::string_view name, xcb_atom_t type,
             std::initializer_list<std::uint32_t> data) {
        set(window, name, type, std::span(data.begin(), data.size()));
    }
    bool protocol(xcb_window_t window, std::string_view name) {
        const auto values = property(window, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM);
        return std::find(values.begin(), values.end(), atom(name)) != values.end();
    }
    void message(xcb_window_t window, std::string_view name, xcb_timestamp_t time) {
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = window;
        event.type = atom("WM_PROTOCOLS");
        event.data.data32[0] = atom(name);
        event.data.data32[1] = time;
        api_.send_event(connection_, false, window, XCB_EVENT_MASK_NO_EVENT,
                        reinterpret_cast<const char*>(&event));
    }
    void lists() {
        set(screen_->root, "_NET_CLIENT_LIST", XCB_ATOM_WINDOW, order_);
        const auto tree = reply<xcb_query_tree_reply_t>(api_.query_tree(connection_, screen_->root));
        const auto count = api_.query_tree_children_length(tree.get());
        if (count < 0 || count > 4096)
            throw std::runtime_error("DESKTOP_WINDOW_LIMIT");
        const auto* windows = api_.query_tree_children(tree.get());
        std::vector<std::uint32_t> stack;
        stack.reserve(clients_.size());
        for (int i = 0; i < count; ++i)
            if (clients_.contains(windows[i]))
                stack.push_back(windows[i]);
        set(screen_->root, "_NET_CLIENT_LIST_STACKING", XCB_ATOM_WINDOW, stack);
        set(screen_->root, "_NET_ACTIVE_WINDOW", XCB_ATOM_WINDOW, {active_});
    }
    Geometry geometry(const Client& client) const {
        auto value = client.normal;
        if (client.fullscreen || client.horizontal) {
            value.x = 0;
            value.width = screen_->width_in_pixels;
        }
        if (client.fullscreen || client.vertical) {
            value.y = 0;
            value.height = screen_->height_in_pixels;
        }
        return value;
    }
    void constrain(Geometry& value) const {
        value.width = std::clamp(value.width, 1u, static_cast<unsigned>(screen_->width_in_pixels));
        value.height = std::clamp(value.height, 1u, static_cast<unsigned>(screen_->height_in_pixels));
        value.x = std::clamp(value.x, 0, static_cast<int>(screen_->width_in_pixels - value.width));
        value.y = std::clamp(value.y, 0, static_cast<int>(screen_->height_in_pixels - value.height));
    }
    void configure(xcb_window_t window, const Client& client) {
        const auto value = geometry(client);
        const std::array<std::uint32_t, 5> data{static_cast<std::uint32_t>(value.x),
                                                static_cast<std::uint32_t>(value.y), value.width,
                                                value.height, 0};
        api_.configure_window(connection_, window,
                              XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                  XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH,
                              data.data());
        xcb_configure_notify_event_t notify{};
        notify.response_type = XCB_CONFIGURE_NOTIFY;
        notify.event = window;
        notify.window = window;
        notify.x = static_cast<std::int16_t>(value.x);
        notify.y = static_cast<std::int16_t>(value.y);
        notify.width = static_cast<std::uint16_t>(value.width);
        notify.height = static_cast<std::uint16_t>(value.height);
        api_.send_event(connection_, false, window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                        reinterpret_cast<const char*>(&notify));
    }
    void state(xcb_window_t window, const Client& client) {
        std::vector<std::uint32_t> states;
        if (client.fullscreen)
            states.push_back(atom("_NET_WM_STATE_FULLSCREEN"));
        if (client.horizontal)
            states.push_back(atom("_NET_WM_STATE_MAXIMIZED_HORZ"));
        if (client.vertical)
            states.push_back(atom("_NET_WM_STATE_MAXIMIZED_VERT"));
        if (client.minimized)
            states.push_back(atom("_NET_WM_STATE_HIDDEN"));
        set(window, "_NET_WM_STATE", XCB_ATOM_ATOM, states);
        set(window, "WM_STATE", atom("WM_STATE"), {client.minimized ? 3u : 1u, 0});
    }
    void focus(xcb_window_t window, xcb_timestamp_t time = XCB_CURRENT_TIME) {
        const auto found = clients_.find(window);
        if (found == clients_.end())
            return;
        if (found->second.minimized) {
            found->second.minimized = false;
            api_.map_window(connection_, window);
            state(window, found->second);
        }
        const std::uint32_t above = XCB_STACK_MODE_ABOVE;
        api_.configure_window(connection_, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        const auto hints = property(window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 9);
        if (hints.size() < 2 || !(hints[0] & 1u) || hints[1])
            api_.set_input_focus(connection_, XCB_INPUT_FOCUS_POINTER_ROOT, window, time);
        if (protocol(window, "WM_TAKE_FOCUS"))
            message(window, "WM_TAKE_FOCUS", time);
        active_ = window;
        lists();
    }
    void manage(xcb_window_t window, bool map) {
        if (window == support_)
            return;
        if (clients_.contains(window)) {
            if (map) {
                api_.map_window(connection_, window);
                focus(window);
            }
            return;
        }
        const auto attributes =
            reply<xcb_get_window_attributes_reply_t>(api_.get_window_attributes(connection_, window));
        if (attributes->override_redirect || attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT)
            return;
        if (clients_.size() >= 256)
            throw std::runtime_error("DESKTOP_MANAGED_WINDOW_LIMIT");
        const auto box = reply<xcb_get_geometry_reply_t>(api_.get_geometry(connection_, window));
        Client client;
        client.normal = {box->x, box->y, box->width, box->height};
        constrain(client.normal);
        for (const auto value : property(window, atom("_NET_WM_STATE"), XCB_ATOM_ATOM)) {
            client.fullscreen |= value == atom("_NET_WM_STATE_FULLSCREEN");
            client.horizontal |= value == atom("_NET_WM_STATE_MAXIMIZED_HORZ");
            client.vertical |= value == atom("_NET_WM_STATE_MAXIMIZED_VERT");
        }
        clients_.emplace(window, client);
        order_.push_back(window);
        const std::uint32_t events = XCB_EVENT_MASK_FOCUS_CHANGE;
        api_.change_window_attributes(connection_, window, XCB_CW_EVENT_MASK, &events);
        api_.grab_button(connection_, false, window, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC,
                         XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
        set(window, "_NET_FRAME_EXTENTS", XCB_ATOM_CARDINAL, {0, 0, 0, 0});
        set(window, "_NET_WM_DESKTOP", XCB_ATOM_CARDINAL, {0});
        set(window, "_NET_WM_ALLOWED_ACTIONS", XCB_ATOM_ATOM,
            {atom("_NET_WM_ACTION_MINIMIZE"), atom("_NET_WM_ACTION_MAXIMIZE_HORZ"),
             atom("_NET_WM_ACTION_MAXIMIZE_VERT"), atom("_NET_WM_ACTION_FULLSCREEN"),
             atom("_NET_WM_ACTION_CLOSE")});
        configure(window, client);
        state(window, client);
        if (map)
            api_.map_window(connection_, window);
        focus(window);
    }
    void remove(xcb_window_t window) {
        if (!clients_.erase(window))
            return;
        std::erase(order_, window);
        if (active_ == window)
            active_ = XCB_NONE;
        lists();
        if (!active_)
            for (auto it = order_.rbegin(); it != order_.rend(); ++it)
                if (!clients_.at(*it).minimized) {
                    focus(*it);
                    break;
                }
    }
    void client_message(const xcb_client_message_event_t& event) {
        if (event.format != 32)
            return;
        if (event.type == atom("_NET_REQUEST_FRAME_EXTENTS")) {
            set(event.window, "_NET_FRAME_EXTENTS", XCB_ATOM_CARDINAL, {0, 0, 0, 0});
            return;
        }
        const auto found = clients_.find(event.window);
        if (found == clients_.end())
            return;
        auto& client = found->second;
        if (event.type == atom("_NET_ACTIVE_WINDOW"))
            focus(event.window, event.data.data32[1]);
        else if (event.type == atom("_NET_CLOSE_WINDOW")) {
            if (protocol(event.window, "WM_DELETE_WINDOW"))
                message(event.window, "WM_DELETE_WINDOW", event.data.data32[0]);
        } else if (event.type == atom("WM_CHANGE_STATE") && event.data.data32[0] == 3 && !client.minimized) {
            client.minimized = true;
            state(event.window, client);
            api_.unmap_window(connection_, event.window);
            if (active_ == event.window) {
                active_ = XCB_NONE;
                lists();
            }
        } else if (event.type == atom("_NET_WM_STATE") && event.data.data32[0] <= 2) {
            const auto action = event.data.data32[0];
            for (unsigned i = 1; i <= 2; ++i) {
                const auto name = event.data.data32[i];
                if (i == 2 && name == event.data.data32[1])
                    continue;
                bool* value = name == atom("_NET_WM_STATE_FULLSCREEN")       ? &client.fullscreen
                              : name == atom("_NET_WM_STATE_MAXIMIZED_HORZ") ? &client.horizontal
                              : name == atom("_NET_WM_STATE_MAXIMIZED_VERT") ? &client.vertical
                                                                             : nullptr;
                if (value)
                    *value = action == 2 ? !*value : action == 1;
            }
            configure(event.window, client);
            state(event.window, client);
        }
    }
    void event(xcb_generic_event_t* value) {
        switch (value->response_type & 0x7f) {
        case XCB_MAP_REQUEST:
            manage(reinterpret_cast<xcb_map_request_event_t*>(value)->window, true);
            break;
        case XCB_DESTROY_NOTIFY:
            remove(reinterpret_cast<xcb_destroy_notify_event_t*>(value)->window);
            break;
        case XCB_UNMAP_NOTIFY: {
            const auto window = reinterpret_cast<xcb_unmap_notify_event_t*>(value)->window;
            if (const auto found = clients_.find(window);
                found != clients_.end() && !found->second.minimized) {
                api_.delete_property(connection_, window, atom("WM_STATE"));
                api_.delete_property(connection_, window, atom("_NET_WM_DESKTOP"));
                remove(window);
            }
            break;
        }
        case XCB_CONFIGURE_REQUEST: {
            const auto& request = *reinterpret_cast<xcb_configure_request_event_t*>(value);
            const auto found = clients_.find(request.window);
            if (found == clients_.end()) {
                std::vector<std::uint32_t> data;
                if (request.value_mask & XCB_CONFIG_WINDOW_X)
                    data.push_back(static_cast<std::uint32_t>(request.x));
                if (request.value_mask & XCB_CONFIG_WINDOW_Y)
                    data.push_back(static_cast<std::uint32_t>(request.y));
                if (request.value_mask & XCB_CONFIG_WINDOW_WIDTH)
                    data.push_back(request.width);
                if (request.value_mask & XCB_CONFIG_WINDOW_HEIGHT)
                    data.push_back(request.height);
                if (request.value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
                    data.push_back(request.border_width);
                if (request.value_mask & XCB_CONFIG_WINDOW_SIBLING)
                    data.push_back(request.sibling);
                if (request.value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
                    data.push_back(request.stack_mode);
                api_.configure_window(connection_, request.window, request.value_mask, data.data());
            } else {
                auto& client = found->second;
                if (!client.fullscreen && !client.horizontal && !client.vertical) {
                    if (request.value_mask & XCB_CONFIG_WINDOW_X)
                        client.normal.x = request.x;
                    if (request.value_mask & XCB_CONFIG_WINDOW_Y)
                        client.normal.y = request.y;
                    if (request.value_mask & XCB_CONFIG_WINDOW_WIDTH)
                        client.normal.width = request.width;
                    if (request.value_mask & XCB_CONFIG_WINDOW_HEIGHT)
                        client.normal.height = request.height;
                    constrain(client.normal);
                }
                configure(request.window, client);
                std::array<std::uint32_t, 2> data{};
                unsigned count = 0;
                if (request.value_mask & XCB_CONFIG_WINDOW_SIBLING)
                    data[count++] = request.sibling;
                if (request.value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
                    data[count++] = request.stack_mode;
                if (count)
                    api_.configure_window(connection_, request.window,
                                          request.value_mask &
                                              (XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE),
                                          data.data());
                lists();
            }
            break;
        }
        case XCB_CONFIGURE_NOTIFY:
            if (clients_.contains(reinterpret_cast<xcb_configure_notify_event_t*>(value)->window))
                lists();
            break;
        case XCB_CLIENT_MESSAGE:
            client_message(*reinterpret_cast<xcb_client_message_event_t*>(value));
            break;
        case XCB_BUTTON_PRESS: {
            const auto& press = *reinterpret_cast<xcb_button_press_event_t*>(value);
            try {
                focus(press.event, press.time);
            } catch (const Gone&) {
            }
            api_.allow_events(connection_, XCB_ALLOW_REPLAY_POINTER, press.time);
            break;
        }
        case XCB_FOCUS_IN: {
            const auto& gained = *reinterpret_cast<xcb_focus_in_event_t*>(value);
            if (clients_.contains(gained.event) && active_ != gained.event &&
                gained.mode == XCB_NOTIFY_MODE_NORMAL) {
                active_ = gained.event;
                lists();
            }
            break;
        }
        case XCB_SELECTION_CLEAR:
            throw std::runtime_error("DESKTOP_WINDOW_MANAGER_OWNERSHIP_LOST");
        default:
            break; // BadWindow from a concurrently destroyed client is harmless.
        }
    }

  public:
    Manager(int socket, const X11Cookie& cookie, std::atomic<bool>& stop,
            std::atomic<std::int64_t>& heartbeat)
        : socket_{socket}, stop_(stop), heartbeat_(heartbeat) {
        xcb_auth_info_t auth{18, const_cast<char*>("MIT-MAGIC-COOKIE-1"), 16,
                             reinterpret_cast<char*>(const_cast<std::uint8_t*>(cookie.data()))};
        connection_ = api_.connect_to_fd(socket, &auth); // Takes ownership of socket, even on failure.
        socket_.value = -1;
        try {
            check();
            screen_ = api_.setup_roots_iterator(api_.get_setup(connection_)).data;
            if (!screen_)
                throw std::runtime_error("DESKTOP_X11_SCREEN_UNAVAILABLE");
            const std::uint32_t events =
                XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
            const auto ownership =
                api_.change_window_attributes_checked(connection_, screen_->root, XCB_CW_EVENT_MASK, &events);
            (void)reply<xcb_get_input_focus_reply_t>(api_.get_input_focus(connection_));
            Reply<xcb_generic_error_t> error(api_.request_check(connection_, ownership));
            if (error)
                throw std::runtime_error("DESKTOP_EXISTING_WINDOW_MANAGER");
            if (reply<xcb_get_selection_owner_reply_t>(api_.get_selection_owner(connection_, atom("WM_S0")))
                    ->owner)
                throw std::runtime_error("DESKTOP_EXISTING_WINDOW_MANAGER");
            support_ = api_.generate_id(connection_);
            api_.create_window(connection_, XCB_COPY_FROM_PARENT, support_, screen_->root, -1, -1, 1, 1, 0,
                               XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
            api_.set_selection_owner(connection_, support_, atom("WM_S0"), XCB_CURRENT_TIME);
            if (reply<xcb_get_selection_owner_reply_t>(api_.get_selection_owner(connection_, atom("WM_S0")))
                    ->owner != support_)
                throw std::runtime_error("DESKTOP_WINDOW_MANAGER_OWNERSHIP_LOST");
            set(screen_->root, "_NET_SUPPORTING_WM_CHECK", XCB_ATOM_WINDOW, {support_});
            set(support_, "_NET_SUPPORTING_WM_CHECK", XCB_ATOM_WINDOW, {support_});
            constexpr std::string_view name = "Devbox native desktop";
            api_.change_property(connection_, XCB_PROP_MODE_REPLACE, support_, atom("_NET_WM_NAME"),
                                 atom("UTF8_STRING"), 8, static_cast<std::uint32_t>(name.size()),
                                 name.data());
            std::vector<std::uint32_t> supported;
            for (const auto key : {"_NET_SUPPORTED",
                                   "_NET_SUPPORTING_WM_CHECK",
                                   "_NET_CLIENT_LIST",
                                   "_NET_CLIENT_LIST_STACKING",
                                   "_NET_ACTIVE_WINDOW",
                                   "_NET_CLOSE_WINDOW",
                                   "_NET_WM_STATE",
                                   "_NET_WM_STATE_FULLSCREEN",
                                   "_NET_WM_STATE_MAXIMIZED_HORZ",
                                   "_NET_WM_STATE_MAXIMIZED_VERT",
                                   "_NET_WM_STATE_HIDDEN",
                                   "_NET_FRAME_EXTENTS",
                                   "_NET_REQUEST_FRAME_EXTENTS",
                                   "_NET_WM_ALLOWED_ACTIONS",
                                   "_NET_WM_ACTION_MINIMIZE",
                                   "_NET_WM_ACTION_MAXIMIZE_HORZ",
                                   "_NET_WM_ACTION_MAXIMIZE_VERT",
                                   "_NET_WM_ACTION_FULLSCREEN",
                                   "_NET_WM_ACTION_CLOSE",
                                   "_NET_NUMBER_OF_DESKTOPS",
                                   "_NET_CURRENT_DESKTOP",
                                   "_NET_DESKTOP_GEOMETRY",
                                   "_NET_DESKTOP_VIEWPORT",
                                   "_NET_WORKAREA",
                                   "_NET_WM_DESKTOP"})
                supported.push_back(atom(key));
            set(screen_->root, "_NET_SUPPORTED", XCB_ATOM_ATOM, supported);
            set(screen_->root, "_NET_NUMBER_OF_DESKTOPS", XCB_ATOM_CARDINAL, {1});
            set(screen_->root, "_NET_CURRENT_DESKTOP", XCB_ATOM_CARDINAL, {0});
            set(screen_->root, "_NET_DESKTOP_GEOMETRY", XCB_ATOM_CARDINAL,
                {screen_->width_in_pixels, screen_->height_in_pixels});
            set(screen_->root, "_NET_DESKTOP_VIEWPORT", XCB_ATOM_CARDINAL, {0, 0});
            set(screen_->root, "_NET_WORKAREA", XCB_ATOM_CARDINAL,
                {0, 0, screen_->width_in_pixels, screen_->height_in_pixels});
            xcb_client_message_event_t announcement{};
            announcement.response_type = XCB_CLIENT_MESSAGE;
            announcement.format = 32;
            announcement.window = screen_->root;
            announcement.type = atom("MANAGER");
            announcement.data.data32[1] = atom("WM_S0");
            announcement.data.data32[2] = support_;
            api_.send_event(connection_, false, screen_->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                            reinterpret_cast<const char*>(&announcement));
            const auto tree = reply<xcb_query_tree_reply_t>(api_.query_tree(connection_, screen_->root));
            const auto count = api_.query_tree_children_length(tree.get());
            if (count < 0 || count > 4096)
                throw std::runtime_error("DESKTOP_WINDOW_LIMIT");
            const auto* windows = api_.query_tree_children(tree.get());
            for (int i = 0; i < count; ++i) {
                try {
                    if (reply<xcb_get_window_attributes_reply_t>(
                            api_.get_window_attributes(connection_, windows[i]))
                            ->map_state == XCB_MAP_STATE_VIEWABLE)
                        manage(windows[i], false);
                } catch (const Gone&) {
                }
            }
            lists();
            api_.flush(connection_);
        } catch (...) {
            api_.disconnect(connection_);
            connection_ = nullptr;
            throw;
        }
    }
    ~Manager() {
        if (connection_)
            api_.disconnect(connection_);
    }
    void run() {
        while (!stop_.load()) {
            check();
            heartbeat_.store(ticks());
            unsigned events = 0;
            while (events++ < 64) {
                Reply<xcb_generic_event_t> value(api_.poll_for_event(connection_));
                if (!value)
                    break;
                try {
                    event(value.get());
                } catch (const Gone&) {
                }
                check();
                heartbeat_.store(ticks());
            }
            api_.flush(connection_);
            if (events < 64) {
                pollfd descriptor{api_.get_file_descriptor(connection_), POLLIN, 0};
                // Incoming events wake immediately. Cancellation shuts down the
                // pinned socket, so idle health sampling needs only one wake/s.
                ::poll(&descriptor, 1, 1000);
            }
        }
    }
};
} // namespace
struct DesktopWindowManager::State {
    std::atomic<bool> stopped{false}, ready{false}, exited{false};
    std::atomic<std::int64_t> heartbeat{ticks()};
    std::mutex mutex;
    int pinned_socket = -1;
    std::string error;
    std::thread worker;
    void close() noexcept {
        stopped.store(true);
        {
            std::lock_guard lock(mutex);
            // A duplicated descriptor pins the socket while XCB owns/closes its
            // original. Cancellation can never act on a reused descriptor.
            if (pinned_socket >= 0)
                ::shutdown(pinned_socket, SHUT_RDWR);
        }
        if (worker.joinable())
            worker.join();
        if (pinned_socket >= 0) {
            ::close(pinned_socket);
            pinned_socket = -1;
        }
    }
    ~State() {
        close();
    }
};
DesktopWindowManager::DesktopWindowManager(unsigned display, const X11Cookie& cookie,
                                           const std::function<bool()>& stop, std::uint32_t expected_peer_pid)
    : state_(std::make_unique<State>()) {
    auto& state = *state_;
    state.worker = std::thread([&state, display, cookie, expected_peer_pid] {
        (void)::pthread_setname_np(::pthread_self(), "devbox-x11-wm");
        try {
            // XCB can write while cancellation shuts down our pinned socket.
            // Block its thread-directed SIGPIPE for this owned worker's entire
            // lifetime: a failed write must become an XCB error, not terminate
            // the desktop supervisor. Do not change the process-wide policy or
            // unblock a pending signal before this thread exits.
            sigset_t blocked;
            ::sigemptyset(&blocked);
            ::sigaddset(&blocked, SIGPIPE);
            if (::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0)
                throw std::runtime_error("DESKTOP_WINDOW_MANAGER_SIGNAL_MASK_FAILED");
            const auto fd = connect_private_x11(
                display, Steady::now() + std::chrono::seconds(2), [&] { return state.stopped.load(); },
                expected_peer_pid);
            {
                std::lock_guard lock(state.mutex);
                state.pinned_socket = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
                if (state.pinned_socket < 0) {
                    ::close(fd);
                    throw std::runtime_error("DESKTOP_X11_SOCKET_PIN_FAILED");
                }
                if (state.stopped.load()) {
                    ::close(fd);
                    throw std::runtime_error("DESKTOP_WINDOW_MANAGER_CANCELLED");
                }
            }
            Manager manager(fd, cookie, state.stopped, state.heartbeat);
            state.heartbeat.store(ticks());
            state.ready.store(true);
            manager.run();
        } catch (const std::exception& error) {
            std::lock_guard lock(state.mutex);
            state.error = error.what();
        } catch (...) {
            std::lock_guard lock(state.mutex);
            state.error = "DESKTOP_WINDOW_MANAGER_FAILED";
        }
        state.exited.store(true);
    });
    const auto until = Steady::now() + std::chrono::seconds(5);
    while (!state.ready.load() && !state.exited.load()) {
        if ((stop && stop()) || Steady::now() >= until) {
            state.close();
            throw std::runtime_error("DESKTOP_WINDOW_MANAGER_START_DEADLINE");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (state.exited.load()) {
        state.close();
        throw std::runtime_error(state.error.empty() ? "DESKTOP_WINDOW_MANAGER_FAILED" : state.error);
    }
}
DesktopWindowManager::~DesktopWindowManager() = default;
bool DesktopWindowManager::healthy() const {
    return state_->ready.load() && !state_->exited.load() && ticks() - state_->heartbeat.load() < 5000;
}
#else
struct DesktopWindowManager::State {};
DesktopWindowManager::DesktopWindowManager(unsigned, const X11Cookie&, const std::function<bool()>&,
                                           std::uint32_t) {
    throw std::runtime_error("Native private desktop window management requires Linux");
}
DesktopWindowManager::~DesktopWindowManager() = default;
bool DesktopWindowManager::healthy() const {
    return false;
}
#endif
} // namespace devbox
