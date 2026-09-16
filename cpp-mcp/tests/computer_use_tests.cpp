#include "devbox/computer_use.hpp"
#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include "devbox/oauth.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace devbox;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw Error(message);
}
template <class Function> void rejects(Function function, const std::string& fragment) {
    try {
        function();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(fragment) != std::string::npos, error.what());
        return;
    }
    throw Error("Expected rejection: " + fragment);
}
void contract_checks() {
    Config config;
    config.platform = Platform::detect();
    config.runtime_mode = RuntimeMode::host;
    config.host_exec_enabled = true;
    ToolContract contract(config);
    require(contract.all().size() == 47, "native extension count");
    require(required_tool_scope("host_computer_use") == "mcp:host:exec", "input requires execution scope");
    require(!oauth_scope_allows({"mcp:host:read"}, "mcp:host:exec"), "read scope cannot inject input");
    require(contract.tool("host_computer_windows")["annotations"]["readOnlyHint"] == true,
            "inventory is read-only");
    const auto input = contract.tool("host_computer_use");
    require(input["annotations"]["readOnlyHint"] == false && input["annotations"]["idempotentHint"] == false,
            "computer input cannot be advertised as read-only or safe to repeat");
    for (const auto bad :
         {Json{{"action", "type"}, {"text", std::string(4097, 'x')}},
          Json{{"action", "click"}, {"x", -1}, {"y", 0}}, Json{{"action", "key"}, {"hold_ms", 5001}},
          Json{{"action", "execute_script"}}, Json{{"action", "observe"}, {"command", "anything"}}})
        rejects([&] { contract.arguments("host_computer_use", bad); }, "arguments");
#ifndef _WIN32
    ComputerUse computer;
    require(computer.windows(Json::object(), {})["supported"] == false, "unsupported platform is explicit");
    rejects([&] { computer.perform(Json{{"action", "observe"}}, {}); }, "COMPUTER_UNSUPPORTED");
#endif
    std::cout << "computer schema, bounds, scope and platform contract passed\n";
}
#ifdef _WIN32
struct Fixture {
    HWND window = nullptr, edit = nullptr;
    std::atomic_uint down{0}, up{0}, double_clicks{0}, drag_moves{0}, keys{0};
    std::atomic_int wheel{0};
    std::atomic_bool control_a{false};
    WNDPROC original_edit = nullptr;
    std::thread thread;
    std::string title = "Devbox Native CUA Test " + std::to_string(GetCurrentProcessId());
    static LRESULT CALLBACK edit_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Fixture*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        if (message == WM_KEYDOWN && wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
            self->control_a = true;
        return CallWindowProcW(self->original_edit, window, message, wparam, lparam);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Fixture* self = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Fixture*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_CREATE:
            self->edit =
                CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 20, 45,
                                250, 50, window, nullptr, GetModuleHandleW(nullptr), nullptr);
            self->original_edit = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(self->edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_procedure)));
            SetTimer(window, 1, 120000, nullptr);
            return 0;
        case WM_LBUTTONDOWN:
            ++self->down;
            SetFocus(window);
            SetCapture(window);
            return 0;
        case WM_LBUTTONDBLCLK:
            ++self->double_clicks;
            SetCapture(window);
            return 0;
        case WM_LBUTTONUP:
            ++self->up;
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE:
            if (wparam & MK_LBUTTON)
                ++self->drag_moves;
            return 0;
        case WM_MOUSEWHEEL:
            self->wheel += static_cast<short>(HIWORD(wparam));
            return 0;
        case WM_KEYDOWN:
            ++self->keys;
            return 0;
        case WM_TIMER:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            const wchar_t label[] = L"Owned native CUA regression fixture";
            TextOutW(dc, 20, 15, label, static_cast<int>(std::size(label) - 1));
            Rectangle(dc, 290, 110, 600, 360);
            const wchar_t area[] = L"Click, scroll and drag here";
            TextOutW(dc, 310, 130, area, static_cast<int>(std::size(area) - 1));
            EndPaint(window, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }
    Fixture() {
        auto ready = std::make_shared<std::promise<void>>();
        auto future = ready->get_future();
        thread = std::thread([this, ready] {
            try {
                SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                WNDCLASSW cls{};
                cls.lpfnWndProc = procedure;
                cls.hInstance = GetModuleHandleW(nullptr);
                cls.lpszClassName = L"DevboxNativeCuaFixture";
                cls.style = CS_DBLCLKS;
                cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
                require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                        "fixture class");
                window = CreateWindowExW(0, cls.lpszClassName, wide(title).c_str(), WS_OVERLAPPEDWINDOW, 80,
                                         80, 650, 450, nullptr, nullptr, cls.hInstance, this);
                require(window && edit, "fixture window");
                ShowWindow(window, SW_SHOW);
                UpdateWindow(window);
                ready->set_value();
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            } catch (...) {
                try {
                    ready->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        });
        try {
            future.get();
        } catch (...) {
            thread.join();
            throw;
        }
    }
    ~Fixture() {
        DWORD owner = 0;
        if (IsWindow(window) && GetWindowThreadProcessId(window, &owner) && owner == GetCurrentProcessId())
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (thread.joinable())
            thread.join();
    }
    std::string text() const {
        wchar_t buffer[1024]{};
        GetWindowTextW(edit, buffer, 1024);
        return narrow(buffer);
    }
};
void native_checks() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Fixture fixture;
    ComputerUse computer;
    const auto inventory = computer.windows(Json{{"title_contains", fixture.title}}, {});
    require(inventory["windows"].size() == 1, "exact owned fixture window discovered");
    const auto id = json_string(inventory["windows"][0], "window_id");
    ImageCapture frame;
    auto observe = [&] {
        frame = computer.perform(Json{{"action", "observe"}, {"window_id", id}, {"max_width", 640}}, {});
    };
    observe();
    validate_capture_image(frame.image, frame.mime_type);
    auto position = [&](int x, int y) {
        POINT point{x, y};
        ClientToScreen(fixture.window, &point);
        const auto& m = frame.metadata;
        return Json{{"x", (point.x - m["source_left"].get<int>()) * m["image_width"].get<int>() /
                              m["source_width"].get<int>()},
                    {"y", (point.y - m["source_top"].get<int>()) * m["image_height"].get<int>() /
                              m["source_height"].get<int>()}};
    };
    auto act = [&](Json args) {
        args["observation_id"] = frame.metadata["observation_id"];
        frame = computer.perform(args, {});
        validate_capture_image(frame.image, frame.mime_type);
    };
    HWND occluder =
        CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"Owned CUA occlusion fixture", WS_POPUP | WS_VISIBLE, 180,
                        180, 180, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(occluder != nullptr, "owned occlusion fixture");
    ScopeExit remove_occluder([&] { DestroyWindow(occluder); });
    rejects([&] { computer.perform(Json{{"action", "observe"}, {"window_id", id}}, {}); }, "WINDOW_OCCLUDED");
    DestroyWindow(occluder);
    remove_occluder.disarm();
    observe();
    const auto ready_id = frame.metadata["observation_id"];
    const auto foreground_before = GetForegroundWindow();
    const auto keys_before = fixture.keys.load();
    for (const auto& chord :
         {Json::array({"CTRL", "ESC"}), Json::array({"CTRL", "SHIFT", "ESC"}), Json::array({"ALT", "TAB"}),
          Json::array({"ALT", "ESC"}), Json::array({"CTRL", "ALT", "DELETE"})}) {
        rejects(
            [&] {
                computer.perform(Json{{"action", "key"}, {"observation_id", ready_id}, {"keys", chord}}, {});
            },
            "SYSTEM_SHORTCUT_DENIED");
    }
    require(GetForegroundWindow() == foreground_before && fixture.keys == keys_before,
            "system-global chords are rejected before input or foreground changes");
    rejects(
        [&] {
            computer.perform(Json{{"action", "click"},
                                  {"observation_id", ready_id},
                                  {"x", 10},
                                  {"y", 10},
                                  {"keys", {"SHIFT"}}},
                             {});
        },
        "ARGUMENTS_INVALID");
    rejects(
        [&] {
            computer.perform(Json{{"action", "click"}, {"observation_id", ready_id}, {"x", 32767}, {"y", 0}},
                             {});
        },
        "OUTSIDE_IMAGE");
    auto cursor_position = position(355, 185);
    const int cursor_x = frame.metadata["source_left"].get<int>() +
                         cursor_position["x"].get<int>() * frame.metadata["source_width"].get<int>() /
                             frame.metadata["image_width"].get<int>();
    const int cursor_y = frame.metadata["source_top"].get<int>() +
                         cursor_position["y"].get<int>() * frame.metadata["source_height"].get<int>() /
                             frame.metadata["image_height"].get<int>();
    cursor_position["action"] = "move";
    act(cursor_position);
    POINT actual_cursor{};
    require(GetCursorPos(&actual_cursor) && actual_cursor.x == cursor_x && actual_cursor.y == cursor_y,
            "exact cursor coordinates: expected " + std::to_string(cursor_x) + "," +
                std::to_string(cursor_y) + " observed " + std::to_string(actual_cursor.x) + "," +
                std::to_string(actual_cursor.y));
    const auto click_id = frame.metadata["observation_id"];
    auto click = position(55, 65);
    click["action"] = "click";
    act(click);
    rejects(
        [&] {
            computer.perform(Json{{"action", "click"}, {"observation_id", click_id}, {"x", 10}, {"y", 10}},
                             {});
        },
        "STALE_OBSERVATION");
    act(Json{{"action", "type"}, {"text", "Cua \xce\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80"}});
    require(fixture.text() == "Cua \xce\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80",
            "native Unicode input including surrogate pair");
    act(Json{{"action", "key"}, {"keys", {"CTRL", "A"}}});
    require(fixture.control_a, "Ctrl+A arrived with the Control modifier held");
    // The classic Win32 EDIT does not implement Ctrl+A; test its supported selection keys.
    act(Json{{"action", "key"}, {"keys", {"HOME"}}});
    act(Json{{"action", "key"}, {"keys", {"SHIFT", "END"}}});
    act(Json{{"action", "key"}, {"keys", {"BACKSPACE"}}});
    require(fixture.text().empty(), "native chord and key delivery");
    auto area = position(350, 180);
    area["action"] = "click";
    act(area);
    auto scroll = position(350, 180);
    scroll.update(Json{{"action", "scroll"}, {"scroll_y", 2}});
    act(scroll);
    require(fixture.wheel == -2 * WHEEL_DELTA, "wheel direction and amount");
    act(Json{{"action", "drag"},
             {"path", {position(330, 180), position(420, 230), position(500, 290)}},
             {"duration_ms", 300}});
    require(fixture.drag_moves >= 2 && fixture.up >= 2, "drag path and mouse release");
    auto twice = position(360, 185);
    twice["action"] = "double_click";
    act(twice);
    require(fixture.double_clicks > 0, "double click delivered");
    auto cancellation = std::make_shared<Cancellation>();
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancellation->cancel();
    });
    rejects(
        [&] {
            computer.perform(Json{{"action", "key"},
                                  {"keys", {"RIGHT"}},
                                  {"hold_ms", 5000},
                                  {"observation_id", frame.metadata["observation_id"]}},
                             cancellation);
        },
        "OUTCOME_UNKNOWN");
    cancel.join();
    require(!(GetAsyncKeyState(VK_RIGHT) & 0x8000), "cancelled key hold releases its key");
    observe();
    SetWindowTextW(fixture.window, L"Devbox CUA changed title");
    rejects(
        [&] {
            computer.perform(Json{{"action", "move"},
                                  {"x", 20},
                                  {"y", 20},
                                  {"observation_id", frame.metadata["observation_id"]}},
                             {});
        },
        "STALE_OBSERVATION");
    observe();
    SetWindowPos(fixture.window, nullptr, 90, 90, 900, 550, SWP_NOZORDER | SWP_NOACTIVATE);
    rejects(
        [&] {
            computer.perform(Json{{"action", "move"},
                                  {"x", 20},
                                  {"y", 20},
                                  {"observation_id", frame.metadata["observation_id"]}},
                             {});
        },
        "STALE_OBSERVATION");
    observe();
    require(frame.metadata["image_width"] == 640 && frame.metadata["source_width"].get<int>() > 640,
            "scaled screenshot preserves physical coordinate mapping");
    auto scaled_click = position(55, 65);
    scaled_click["action"] = "click";
    act(scaled_click);
    act(Json{{"action", "type"}, {"text", "scaled"}});
    require(fixture.text() == "scaled", "input coordinates map through screenshot scaling");
    auto wrong = id;
    wrong.back() = wrong.back() == '1' ? '2' : '1';
    rejects([&] { computer.perform(Json{{"action", "observe"}, {"window_id", wrong}}, {}); },
            "WINDOW_CHANGED");
    std::cout << "native CUA: image, click, Unicode, chord, key, scroll, drag, double click, cancellation, "
                 "stale IDs, title/bounds and scaling passed\n";
}
#endif
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "native") {
#ifdef _WIN32
            native_checks();
#else
            throw Error("Native Windows fixture cannot run on this platform");
#endif
        } else
            contract_checks();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
