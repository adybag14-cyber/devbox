#include "devbox/computer_use.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <dwmapi.h>
#endif

namespace devbox {
#ifdef _WIN32
namespace {
struct Rectangle {
    int left = 0, top = 0, width = 0, height = 0;
    bool operator==(const Rectangle&) const = default;
};
struct Window {
    HWND handle = nullptr;
    DWORD pid = 0;
    std::uint64_t created = 0;
    std::string id, title;
    Rectangle bounds, raw_bounds;
};
class DpiScope {
    DPI_AWARENESS_CONTEXT previous_;

  public:
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        if (!previous_)
            throw Error("COMPUTER_DPI_UNAVAILABLE: cannot establish physical-pixel coordinates");
    }
    ~DpiScope() {
        SetThreadDpiAwarenessContext(previous_);
    }
};
void check_cancel(const Cancel& cancel) {
    if (cancel)
        cancel->check();
}
void delay(unsigned duration, const Cancel& cancel) {
    const auto until = Clock::now() + Millis(duration);
    while (Clock::now() < until) {
        check_cancel(cancel);
        const auto remaining = std::chrono::duration_cast<Millis>(until - Clock::now());
        std::this_thread::sleep_for(std::max(Millis(1), std::min(Millis(10), remaining)));
    }
    check_cancel(cancel);
}
void require_desktop() {
    const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop)
        throw Error("COMPUTER_DESKTOP_UNAVAILABLE: unlock the interactive desktop first");
    ScopeExit close([&] { CloseDesktop(desktop); });
    std::array<wchar_t, 128> name{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name.data(),
                                   static_cast<DWORD>(name.size() * sizeof(wchar_t)), &needed) ||
        _wcsicmp(name.data(), L"Default") != 0)
        throw Error("COMPUTER_SECURE_DESKTOP: input is unavailable on a locked or secure desktop");
}
std::uint64_t created_at(DWORD pid) {
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process.get(), &created, &exit, &kernel, &user))
        throw Error("COMPUTER_PROCESS_UNAVAILABLE: cannot verify window process identity");
    DWORD own_session = 0, target_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
        !ProcessIdToSessionId(pid, &target_session) || own_session != target_session)
        throw Error("COMPUTER_SESSION_MISMATCH: target is outside the service's interactive session");
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
Window inspect(HWND handle) {
    if (!IsWindow(handle) || !IsWindowVisible(handle) || IsIconic(handle) ||
        GetAncestor(handle, GA_ROOT) != handle)
        throw Error("COMPUTER_WINDOW_UNAVAILABLE: select a visible non-minimized top-level window");
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        throw Error("COMPUTER_WINDOW_UNAVAILABLE: window is cloaked");
    Window result;
    result.handle = handle;
    GetWindowThreadProcessId(handle, &result.pid);
    result.created = created_at(result.pid);
    std::array<wchar_t, 1025> title{};
    GetWindowTextW(handle, title.data(), static_cast<int>(title.size()));
    result.title = narrow(title.data());
    RECT rect{};
    if (!GetWindowRect(handle, &rect))
        throw Error("COMPUTER_WINDOW_UNAVAILABLE: cannot read window bounds");
    result.raw_bounds = {static_cast<int>(rect.left), static_cast<int>(rect.top),
                         static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top)};
    RECT visible{};
    if (SUCCEEDED(DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
        rect = visible;
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const auto right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const auto bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int x = std::max(static_cast<int>(rect.left), left);
    const int y = std::max(static_cast<int>(rect.top), top);
    result.bounds = {x, y, std::min(static_cast<int>(rect.right), right) - x,
                     std::min(static_cast<int>(rect.bottom), bottom) - y};
    if (result.bounds.width <= 0 || result.bounds.height <= 0)
        throw Error("COMPUTER_WINDOW_UNAVAILABLE: window is outside the visible desktop");
    std::ostringstream id;
    id << std::hex << reinterpret_cast<std::uintptr_t>(handle) << ':' << std::dec << result.pid << ':'
       << std::hex << result.created;
    result.id = id.str();
    return result;
}
std::uint64_t parse_unsigned(std::string_view value, int base) {
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number, base);
    if (value.empty() || error != std::errc{} || end != value.data() + value.size() || !number)
        throw Error("COMPUTER_WINDOW_ID: use a window_id returned by host_computer_windows");
    return number;
}
Window resolve(std::string_view id) {
    const auto a = id.find(':'), b = a == id.npos ? id.npos : id.find(':', a + 1);
    if (a == id.npos || b == id.npos || id.find(':', b + 1) != id.npos)
        throw Error("COMPUTER_WINDOW_ID: invalid window identity");
    const auto handle = parse_unsigned(id.substr(0, a), 16);
    const auto pid = parse_unsigned(id.substr(a + 1, b - a - 1), 10);
    const auto created = parse_unsigned(id.substr(b + 1), 16);
    if (handle > UINTPTR_MAX || pid > UINT32_MAX)
        throw Error("COMPUTER_WINDOW_ID: identity is out of range");
    auto window = inspect(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle)));
    if (window.pid != pid || window.created != created || window.id != id)
        throw Error("COMPUTER_WINDOW_CHANGED: window or process instance changed; list windows again");
    return window;
}
bool foreground(HWND handle) {
    const auto current = GetForegroundWindow();
    return current == handle || (current && GetAncestor(current, GA_ROOTOWNER) == handle);
}
void focus(const Window& window, const Cancel& cancel) {
    require_desktop();
    if (foreground(window.handle))
        return;
    const auto current = GetForegroundWindow();
    const auto thread = current ? GetWindowThreadProcessId(current, nullptr) : 0;
    const auto own_thread = GetCurrentThreadId();
    const bool attached = thread && thread != own_thread && AttachThreadInput(own_thread, thread, TRUE);
    ScopeExit detach([&] {
        if (attached)
            AttachThreadInput(own_thread, thread, FALSE);
    });
    BringWindowToTop(window.handle);
    SetForegroundWindow(window.handle);
    delay(50, cancel);
    if (!foreground(window.handle))
        throw Error("COMPUTER_FOCUS_UNAVAILABLE: activate the selected window and observe again");
}
Json window_json(const Window& window) {
    return Json{{"window_id", window.id},         {"pid", window.pid},
                {"title", window.title},          {"left", window.bounds.left},
                {"top", window.bounds.top},       {"width", window.bounds.width},
                {"height", window.bounds.height}, {"foreground", foreground(window.handle)}};
}
class InputLease {
    NativeHandle mutex_;

  public:
    InputLease() {
        require_desktop();
        DWORD session = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
            throw Error("COMPUTER_SESSION_UNAVAILABLE");
        const auto name = wide("Local\\DevboxComputerInput-" + std::to_string(session));
        mutex_.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!mutex_)
            throw Error("COMPUTER_BUSY: cannot acquire desktop input lock");
        const auto result = WaitForSingleObject(mutex_.get(), 0);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
            throw Error("COMPUTER_BUSY: another native computer-use action is running");
    }
    ~InputLease() {
        if (mutex_)
            ReleaseMutex(mutex_.get());
    }
};
void require_unheld_input() {
    for (const int key :
         {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000)
            throw Error("COMPUTER_INPUT_BUSY: release physical mouse buttons and modifiers before input");
}
void ensure_current(const Window& expected, bool compare_title = true) {
    require_desktop();
    const auto live = resolve(expected.id);
    if (live.bounds != expected.bounds || live.raw_bounds != expected.raw_bounds ||
        (compare_title && live.title != expected.title) || !foreground(live.handle))
        throw Error("COMPUTER_STALE_OBSERVATION: window focus, title or bounds changed; observe again");
}
void require_unoccluded(const Window& target) {
    RECT area{target.bounds.left, target.bounds.top, target.bounds.left + target.bounds.width,
              target.bounds.top + target.bounds.height};
    unsigned count = 0;
    for (auto window = GetWindow(target.handle, GW_HWNDPREV); window;
         window = GetWindow(window, GW_HWNDPREV)) {
        if (++count > 512)
            throw Error("COMPUTER_LAYOUT_UNSTABLE: too many overlapping windows");
        if (!IsWindowVisible(window) || IsIconic(window) ||
            GetAncestor(window, GA_ROOTOWNER) == target.handle)
            continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        RECT bounds{}, overlap{};
        if (GetWindowRect(window, &bounds) && IntersectRect(&overlap, &area, &bounds))
            throw Error(
                "COMPUTER_WINDOW_OCCLUDED: another window covers the target; uncover it and observe again");
    }
}
void send_input(INPUT value, bool& attempted) {
    attempted = true;
    if (SendInput(1, &value, sizeof(value)) != 1)
        throw Error("COMPUTER_INPUT_FAILED: SendInput did not accept the event: " + windows_error());
}
void release_input(INPUT value) noexcept {
    SendInput(1, &value, sizeof(value));
}
INPUT mouse_event(DWORD flags, DWORD data = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    return input;
}
INPUT keyboard_event(WORD key, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_HOME ||
        key == VK_END || key == VK_PRIOR || key == VK_NEXT || key == VK_INSERT || key == VK_DELETE)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return input;
}
WORD key_code(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const std::map<std::string, WORD> names{
        {"CTRL", VK_CONTROL},  {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU},       {"SHIFT", VK_SHIFT},
        {"ENTER", VK_RETURN},  {"RETURN", VK_RETURN},   {"TAB", VK_TAB},        {"ESC", VK_ESCAPE},
        {"ESCAPE", VK_ESCAPE}, {"SPACE", VK_SPACE},     {"BACKSPACE", VK_BACK}, {"DELETE", VK_DELETE},
        {"INSERT", VK_INSERT}, {"LEFT", VK_LEFT},       {"RIGHT", VK_RIGHT},    {"UP", VK_UP},
        {"DOWN", VK_DOWN},     {"HOME", VK_HOME},       {"END", VK_END},        {"PAGEUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT}, {"PLUS", VK_OEM_PLUS},   {"MINUS", VK_OEM_MINUS}};
    if (const auto it = names.find(name); it != names.end())
        return it->second;
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return static_cast<WORD>(name[0]);
    if (name.size() >= 2 && name[0] == 'F') {
        const auto number = parse_unsigned(std::string_view(name).substr(1), 10);
        if (number <= 12)
            return static_cast<WORD>(VK_F1 + number - 1);
    }
    throw Error("COMPUTER_KEY_UNSUPPORTED: use a documented key name; system-global keys are unavailable");
}
} // namespace
#endif

struct ComputerUse::Impl {
    std::mutex operation;
    std::string broker;
#ifdef _WIN32
    struct Observation {
        Window window;
        Clock::time_point created;
        unsigned image_width, image_height, max_width, quality;
    };
    std::map<std::string, Observation> observations;
#endif
};
ComputerUse::ComputerUse(bool allow_broker) : impl_(std::make_unique<Impl>()) {
    if (allow_broker)
        impl_->broker = env_or("DEVBOX_COMPUTER_USE_PIPE", "");
}
ComputerUse::~ComputerUse() = default;

Json ComputerUse::windows(const Json& arguments, const Cancel& cancel) {
#ifdef _WIN32
    std::unique_lock operation(impl_->operation, std::try_to_lock);
    if (!operation.owns_lock())
        throw Error("COMPUTER_BUSY: another computer-use call is running");
    if (!impl_->broker.empty())
        return computer_broker_call(impl_->broker, "windows", arguments, cancel).at("result");
    DpiScope dpi;
    require_desktop();
    struct Enumeration {
        Json values = Json::array();
        std::string filter;
        Cancel cancel;
        std::exception_ptr error;
        bool truncated = false;
    } state;
    state.filter = lower(json_string(arguments, "title_contains"));
    state.cancel = cancel;
    EnumWindows(
        [](HWND handle, LPARAM raw) -> BOOL {
            auto& enumeration = *reinterpret_cast<Enumeration*>(raw);
            try {
                check_cancel(enumeration.cancel);
            } catch (...) {
                enumeration.error = std::current_exception();
                return FALSE;
            }
            try {
                auto window = inspect(handle);
                if (window.title.empty() ||
                    (!enumeration.filter.empty() &&
                     lower(window.title).find(enumeration.filter) == std::string::npos))
                    return TRUE;
                if (enumeration.values.size() == 128) {
                    enumeration.truncated = true;
                    return FALSE;
                }
                enumeration.values.push_back(window_json(window));
            } catch (const Error&) {
                // Inaccessible, disappearing, non-interactive and protected windows are omitted.
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    if (state.error)
        std::rethrow_exception(state.error);
    return Json{{"supported", true},
                {"usage_type", "computer_use"},
                {"windows", state.values},
                {"truncated", state.truncated},
                {"next", "Call host_computer_use action observe with one returned window_id."}};
#else
    (void)arguments;
    (void)cancel;
    return Json{{"supported", false},
                {"usage_type", "computer_use"},
                {"windows", Json::array()},
                {"reason", "Native computer input is supported only on the Windows host runtime."}};
#endif
}

ImageCapture ComputerUse::perform(const Json& arguments, const Cancel& cancel) {
    const auto action = json_string(arguments, "action");
    const std::map<std::string, std::set<std::string>> fields{
        {"observe", {"window_id", "max_width", "quality"}},
        {"click", {"x", "y", "button"}},
        {"double_click", {"x", "y", "button"}},
        {"move", {"x", "y"}},
        {"drag", {"path", "button", "duration_ms"}},
        {"scroll", {"x", "y", "scroll_x", "scroll_y"}},
        {"type", {"text"}},
        {"key", {"keys", "hold_ms"}},
        {"wait", {"duration_ms"}}};
    const auto definition = fields.find(action);
    if (definition == fields.end())
        throw Error("COMPUTER_ACTION_INVALID");
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (it.key() == "action" || it.key() == "settle_ms" ||
            (action != "observe" && (it.key() == "observation_id" || it.key() == "window_id")))
            continue;
        if (!definition->second.contains(it.key()))
            throw Error("COMPUTER_ARGUMENTS_INVALID: field is not applicable to this action: " + it.key());
    }
#ifdef _WIN32
    std::unique_lock operation(impl_->operation, std::try_to_lock);
    if (!operation.owns_lock())
        throw Error("COMPUTER_BUSY: another computer-use call is running");
    if (!impl_->broker.empty()) {
        const auto reply = computer_broker_call(impl_->broker, "perform", arguments, cancel);
        const auto& bytes = reply.at("image").get_binary();
        ImageCapture capture{
            {bytes.begin(), bytes.end()}, reply.at("mime_type").get<std::string>(), reply.at("metadata")};
        validate_capture_image(capture.image, capture.mime_type);
        return capture;
    }
    DpiScope dpi;
    InputLease lease;
    require_unheld_input();
    check_cancel(cancel);
    for (auto it = impl_->observations.begin(); it != impl_->observations.end();) {
        if (Clock::now() - it->second.created > std::chrono::seconds(180))
            it = impl_->observations.erase(it);
        else
            ++it;
    }
    Impl::Observation observation{};
    const auto old_id = json_string(arguments, "observation_id");
    if (action == "observe") {
        observation.window = resolve(json_string(arguments, "window_id"));
        observation.max_width = static_cast<unsigned>(json_uint(arguments, "max_width", 1600));
        observation.quality = static_cast<unsigned>(json_uint(arguments, "quality", 70));
        if (observation.max_width < 640 || observation.max_width > 1920 || observation.quality < 30 ||
            observation.quality > 90)
            throw Error("COMPUTER_IMAGE_BOUNDS");
        focus(observation.window, cancel);
        observation.window = resolve(observation.window.id);
    } else {
        const auto it = impl_->observations.find(old_id);
        if (it == impl_->observations.end())
            throw Error(
                "COMPUTER_STALE_OBSERVATION: observe first; IDs expire and each input consumes its ID");
        observation = it->second;
        const auto requested_window = json_string(arguments, "window_id");
        if (!requested_window.empty() && requested_window != observation.window.id)
            throw Error("COMPUTER_WINDOW_MISMATCH: observation belongs to a different window");
        ensure_current(observation.window);
    }
    auto point = [&](const Json& value) -> POINT {
        if (!value.contains("x") || !value.contains("y") || !value["x"].is_number_integer() ||
            !value["y"].is_number_integer())
            throw Error("COMPUTER_COORDINATES_REQUIRED: provide x/y in the last returned image");
        const auto x = value["x"].get<std::int64_t>(), y = value["y"].get<std::int64_t>();
        if (x < 0 || y < 0 || x >= observation.image_width || y >= observation.image_height)
            throw Error("COMPUTER_COORDINATES_OUTSIDE_IMAGE");
        const auto& bounds = observation.window.bounds;
        return {static_cast<LONG>(bounds.left + x * bounds.width / observation.image_width),
                static_cast<LONG>(bounds.top + y * bounds.height / observation.image_height)};
    };
    const auto duration = json_uint(arguments, "duration_ms", 300);
    const auto hold = json_uint(arguments, "hold_ms", 0);
    const auto settle = json_uint(arguments, "settle_ms", 100);
    if (duration > 5000 || hold > 5000 || settle > 1000)
        throw Error("COMPUTER_DURATION_OUT_OF_RANGE");
    std::vector<POINT> points;
    if (action == "click" || action == "double_click" || action == "move" || action == "scroll")
        points.push_back(point(arguments));
    if (action == "drag") {
        const auto path = arguments.value("path", Json::array());
        if (!path.is_array() || path.size() < 2 || path.size() > 256)
            throw Error("COMPUTER_DRAG_PATH_INVALID");
        for (const auto& entry : path)
            points.push_back(point(entry));
    }
    const auto button = json_string(arguments, "button", "left");
    const auto down = button == "left"    ? MOUSEEVENTF_LEFTDOWN
                      : button == "right" ? MOUSEEVENTF_RIGHTDOWN
                                          : MOUSEEVENTF_MIDDLEDOWN;
    const auto up = button == "left"    ? MOUSEEVENTF_LEFTUP
                    : button == "right" ? MOUSEEVENTF_RIGHTUP
                                        : MOUSEEVENTF_MIDDLEUP;
    if (button != "left" && button != "right" && button != "middle")
        throw Error("COMPUTER_BUTTON_INVALID");
    std::vector<WORD> chord;
    if (action == "key") {
        const auto keys = arguments.value("keys", Json::array());
        if (!keys.is_array() || keys.empty() || keys.size() > 5)
            throw Error("COMPUTER_KEYS_INVALID");
        for (const auto& key : keys) {
            if (!key.is_string())
                throw Error("COMPUTER_KEYS_INVALID");
            const auto code = key_code(key.get<std::string>());
            if (std::find(chord.begin(), chord.end(), code) != chord.end())
                throw Error("COMPUTER_DUPLICATE_KEY");
            chord.push_back(code);
        }
        const auto has = [&](WORD key) { return std::find(chord.begin(), chord.end(), key) != chord.end(); };
        if ((has(VK_CONTROL) && ((has(VK_MENU) && has(VK_DELETE)) || has(VK_ESCAPE))) ||
            (has(VK_MENU) && (has(VK_TAB) || has(VK_ESCAPE))))
            throw Error("COMPUTER_SYSTEM_SHORTCUT_DENIED");
    }
    auto typed = to_utf16(json_string(arguments, "text"));
    if (action == "type" &&
        (typed.empty() || typed.size() > 8192 ||
         std::any_of(typed.begin(), typed.end(), [](char16_t c) { return c < 32 || c == 127; })))
        throw Error("COMPUTER_TEXT_INVALID: use literal text; use key for controls such as Enter and Tab");
    const auto scroll_x = arguments.value("scroll_x", std::int64_t(0));
    const auto scroll_y = arguments.value("scroll_y", std::int64_t(0));
    if (scroll_x < -20 || scroll_x > 20 || scroll_y < -20 || scroll_y > 20 ||
        (action == "scroll" && !scroll_x && !scroll_y))
        throw Error("COMPUTER_SCROLL_INVALID");

    // Consume before the first input: errors and lost acknowledgements cannot replay an input.
    if (action != "observe")
        impl_->observations.erase(old_id);
    for (auto it = impl_->observations.begin(); it != impl_->observations.end();) {
        if (it->second.window.id == observation.window.id)
            it = impl_->observations.erase(it);
        else
            ++it;
    }
    bool attempted = false;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    auto current = [&] {
        check_cancel(cancel);
        if (Clock::now() > deadline)
            throw Error("COMPUTER_ACTION_TIMEOUT");
        ensure_current(observation.window, false);
    };
    auto point_owner = [&](POINT target) {
        current();
        const auto hit = WindowFromPoint(target);
        if (!hit || (GetAncestor(hit, GA_ROOT) != observation.window.handle &&
                     GetAncestor(hit, GA_ROOTOWNER) != observation.window.handle))
            throw Error("COMPUTER_POINT_OCCLUDED: point belongs to another window; observe again");
    };
    auto move = [&](POINT target) {
        point_owner(target);
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (width < 2 || height < 2)
            throw Error("COMPUTER_DISPLAY_UNAVAILABLE");
        auto event = mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                                 MOUSEEVENTF_MOVE_NOCOALESCE);
        event.mi.dx = static_cast<LONG>((static_cast<std::int64_t>(target.x) - left) * 65535 / (width - 1));
        event.mi.dy = static_cast<LONG>((static_cast<std::int64_t>(target.y) - top) * 65535 / (height - 1));
        send_input(event, attempted);
    };
    try {
        if (action == "move")
            move(points.front());
        if (action == "click" || action == "double_click") {
            move(points.front());
            const unsigned count = action == "double_click" ? 2u : 1u;
            for (unsigned i = 0; i < count; ++i) {
                point_owner(points.front());
                bool held = true;
                ScopeExit release([&] {
                    if (held)
                        release_input(mouse_event(up));
                });
                send_input(mouse_event(down), attempted);
                delay(25, cancel);
                send_input(mouse_event(up), attempted);
                held = false;
                if (i + 1 < count)
                    delay(60, cancel);
            }
        }
        if (action == "drag") {
            move(points.front());
            point_owner(points.front());
            bool held = true;
            ScopeExit release([&] {
                if (held)
                    release_input(mouse_event(up));
            });
            send_input(mouse_event(down), attempted);
            const auto started = Clock::now();
            for (std::size_t i = 1; i < points.size(); ++i) {
                const auto target_time =
                    started + Millis(static_cast<Millis::rep>(duration * i / (points.size() - 1)));
                if (Clock::now() < target_time)
                    delay(static_cast<unsigned>(
                              std::chrono::duration_cast<Millis>(target_time - Clock::now()).count()),
                          cancel);
                move(points[i]);
            }
            send_input(mouse_event(up), attempted);
            held = false;
        }
        if (action == "scroll") {
            move(points.front());
            point_owner(points.front());
            if (scroll_y)
                send_input(mouse_event(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-scroll_y * WHEEL_DELTA)),
                           attempted);
            if (scroll_x)
                send_input(mouse_event(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(scroll_x * WHEEL_DELTA)),
                           attempted);
        }
        if (action == "key") {
            std::vector<WORD> held;
            ScopeExit release([&] {
                for (auto it = held.rbegin(); it != held.rend(); ++it)
                    release_input(keyboard_event(*it, true));
            });
            for (const auto key : chord) {
                current();
                held.push_back(key);
                send_input(keyboard_event(key, false), attempted);
            }
            const auto until = Clock::now() + Millis(hold);
            do {
                current();
                delay(10, cancel);
            } while (Clock::now() < until);
            for (auto it = held.rbegin(); it != held.rend(); ++it)
                send_input(keyboard_event(*it, true), attempted);
            held.clear();
        }
        if (action == "type") {
            for (const auto code : typed) {
                current();
                INPUT input{};
                input.type = INPUT_KEYBOARD;
                input.ki.wScan = static_cast<WORD>(code);
                input.ki.dwFlags = KEYEVENTF_UNICODE;
                ScopeExit release([&] {
                    auto up_event = input;
                    up_event.ki.dwFlags |= KEYEVENTF_KEYUP;
                    release_input(up_event);
                });
                send_input(input, attempted);
                input.ki.dwFlags |= KEYEVENTF_KEYUP;
                send_input(input, attempted);
                release.disarm();
            }
        }
        if (action == "wait")
            delay(static_cast<unsigned>(duration), cancel);
        delay(static_cast<unsigned>(settle), cancel);
        check_cancel(cancel);
        auto latest = resolve(observation.window.id);
        if (!foreground(latest.handle))
            throw Error("COMPUTER_FOCUS_CHANGED: observe again");
        require_unoccluded(latest);
        const auto& bounds = latest.bounds;
        auto capture = native_capture_region(bounds.left, bounds.top, bounds.width, bounds.height,
                                             observation.max_width, observation.quality, cancel);
        ensure_current(latest);
        require_unoccluded(latest);
        const auto id = uuid();
        observation.window = latest;
        observation.created = Clock::now();
        observation.image_width = static_cast<unsigned>(json_uint(capture.metadata, "image_width"));
        observation.image_height = static_cast<unsigned>(json_uint(capture.metadata, "image_height"));
        while (impl_->observations.size() >= 32) {
            const auto oldest = std::min_element(
                impl_->observations.begin(), impl_->observations.end(),
                [](const auto& a, const auto& b) { return a.second.created < b.second.created; });
            impl_->observations.erase(oldest);
        }
        impl_->observations.emplace(id, observation);
        capture.metadata["usage_type"] = "computer_use";
        capture.metadata["action"] = action;
        capture.metadata["input_events_sent"] = attempted;
        capture.metadata["window"] = window_json(latest);
        capture.metadata["observation_id"] = id;
        capture.metadata["expires_after_seconds"] = 180;
        capture.metadata["coordinate_space"] = "returned_image_pixels";
        capture.metadata["instruction"] = "Inspect this image. Use this observation_id once for the next "
                                          "action; observe again after an error or window change.";
        return capture;
    } catch (const std::exception& error) {
        if (attempted)
            throw Error("COMPUTER_INPUT_OUTCOME_UNKNOWN: input may have occurred and the observation was "
                        "consumed; observe again before deciding whether to retry. " +
                        std::string(error.what()));
        throw;
    }
#else
    (void)arguments;
    (void)cancel;
    throw Error("COMPUTER_UNSUPPORTED: native computer input requires the Windows host runtime");
#endif
}
} // namespace devbox
