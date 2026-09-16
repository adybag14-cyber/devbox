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
          Json{{"action", "key_sequence"}, {"sequence", Json::array()}},
          Json{{"action", "key_sequence"}, {"sequence", {{{"keys", {"UP"}}, {"duration_ms", 0}}}}},
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
struct BrokerFixture {
    std::string pipe = "Devbox-Cua-Test-" + uuid();
    std::wstring event_name = L"Local\\Devbox-Cua-Stop-" + wide(uuid());
    NativeHandle event, process;
    std::optional<std::string> previous = environment("DEVBOX_COMPUTER_USE_PIPE");
    explicit BrokerFixture(const fs::path& image = executable_path()) {
        event.reset(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
        require(static_cast<bool>(event), "owned broker stop event");
        auto command = L"\"" + image.wstring() + L"\" broker-child " + wide(pipe) + L" " + event_name;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        require(CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &info) != FALSE,
                "spawn exact owned broker fixture");
        process.reset(info.hProcess);
        CloseHandle(info.hThread);
        set_environment("DEVBOX_COMPUTER_USE_PIPE", pipe);
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!WaitNamedPipeW((L"\\\\.\\pipe\\" + wide(pipe)).c_str(), 50)) {
            if (Clock::now() >= deadline || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
                SetEvent(event.get());
                WaitForSingleObject(process.get(), 5000);
                set_environment("DEVBOX_COMPUTER_USE_PIPE", previous);
                throw Error("owned broker did not become ready");
            }
            std::this_thread::sleep_for(Millis(10));
        }
    }
    ~BrokerFixture() {
        set_environment("DEVBOX_COMPUTER_USE_PIPE", previous);
        SetEvent(event.get());
        if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
            // This handle is the exact child returned by our CreateProcess, never a name/PID search.
            TerminateProcess(process.get(), 99);
            WaitForSingleObject(process.get(), 5000);
        }
    }
    void finish() {
        SetEvent(event.get());
        require(WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0, "broker graceful shutdown");
        DWORD status = 99;
        require(GetExitCodeProcess(process.get(), &status) && status == 0, "broker exit status");
    }
};
struct Fixture {
    HWND window = nullptr, edit = nullptr;
    std::atomic_uint down{0}, up{0}, double_clicks{0}, drag_moves{0}, keys{0};
    std::atomic_int wheel{0};
    std::atomic_bool control_a{false};
    std::atomic_uint arrow_up_down{0}, arrow_up_up{0}, arrow_right_down{0}, arrow_right_up{0};
    std::atomic_bool up_held_on_right_down{false};
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
            if (wparam == VK_UP)
                ++self->arrow_up_down;
            if (wparam == VK_RIGHT) {
                ++self->arrow_right_down;
                if (GetKeyState(VK_UP) & 0x8000)
                    self->up_held_on_right_down = true;
            }
            return 0;
        case WM_KEYUP:
            if (wparam == VK_UP)
                ++self->arrow_up_up;
            if (wparam == VK_RIGHT)
                ++self->arrow_right_up;
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
    const auto before_sequence = fixture.keys.load();
    for (const auto entries : {Json::array({Json{{"keys", {"UP"}}, {"duration_ms", 2500}},
                                            Json{{"keys", {"RIGHT"}}, {"duration_ms", 2501}}}),
                               Json::array({Json{{"keys", {"UP"}}, {"duration_ms", 50}},
                                            Json{{"keys", {"CTRL", "ESC"}}, {"duration_ms", 50}}})}) {
        rejects(
            [&] {
                computer.perform(Json{{"action", "key_sequence"},
                                      {"sequence", entries},
                                      {"observation_id", frame.metadata["observation_id"]}},
                                 {});
            },
            entries[0]["duration_ms"] == 2500 ? "SEQUENCE_DURATION" : "SYSTEM_SHORTCUT_DENIED");
        require(fixture.keys == before_sequence, "entire sequence is validated before any key-down");
    }
    act(Json{{"action", "key_sequence"},
             {"sequence",
              {{{"keys", {"UP"}}, {"duration_ms", 80}},
               {{"keys", {"UP", "RIGHT"}}, {"duration_ms", 100}},
               {{"keys", {"UP"}}, {"duration_ms", 80}},
               {{"keys", Json::array()}, {"duration_ms", 30}}}}});
    require(fixture.arrow_up_down == 1 && fixture.arrow_up_up == 1 && fixture.arrow_right_down == 1 &&
                fixture.arrow_right_up == 1 && fixture.up_held_on_right_down,
            "sequence retains acceleration across steering changes and releases each owned key once");
    require(frame.metadata["sequence_segments"] == 4 && frame.metadata["scheduled_duration_ms"] == 290,
            "sequence result records the bounded plan");
    auto sequence_cancel = std::make_shared<Cancellation>();
    std::jthread cancel_sequence([&] {
        std::this_thread::sleep_for(Millis(150));
        sequence_cancel->cancel();
    });
    const auto right_before_cancel = fixture.arrow_right_down.load();
    rejects(
        [&] {
            computer.perform(Json{{"action", "key_sequence"},
                                  {"sequence",
                                   {{{"keys", {"UP"}}, {"duration_ms", 4000}},
                                    {{"keys", {"UP", "RIGHT"}}, {"duration_ms", 500}}}},
                                  {"observation_id", frame.metadata["observation_id"]}},
                             sequence_cancel);
        },
        "OUTCOME_UNKNOWN");
    cancel_sequence.join();
    require(!(GetAsyncKeyState(VK_UP) & 0x8000) && fixture.arrow_right_down == right_before_cancel,
            "sequence cancellation releases retained keys and never executes later segments");
    observe();
    std::jthread change_title([&] {
        std::this_thread::sleep_for(Millis(150));
        SetWindowTextW(fixture.window, L"Devbox sequence target changed");
    });
    rejects(
        [&] {
            computer.perform(Json{{"action", "key_sequence"},
                                  {"sequence", {{{"keys", {"UP"}}, {"duration_ms", 3000}}}},
                                  {"observation_id", frame.metadata["observation_id"]}},
                             {});
        },
        "OUTCOME_UNKNOWN");
    change_title.join();
    require(!(GetAsyncKeyState(VK_UP) & 0x8000), "sequence stops and releases keys on a title change");
    SetWindowTextW(fixture.window, wide(fixture.title).c_str());
    observe();
    auto cancellation = std::make_shared<Cancellation>();
    std::exception_ptr concurrent_failure;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        try {
            rejects([&] { computer.windows(Json::object(), {}); }, "COMPUTER_BUSY");
            rejects([&] { computer.perform(Json{{"action", "observe"}, {"window_id", id}}, {}); },
                    "COMPUTER_BUSY");
        } catch (...) {
            concurrent_failure = std::current_exception();
        }
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
    if (concurrent_failure)
        std::rethrow_exception(concurrent_failure);
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
#ifdef _WIN32
        if (argc == 4 && std::string_view(argv[1]) == "broker-child") {
            NativeHandle stop(OpenEventW(SYNCHRONIZE, FALSE, wide(argv[3]).c_str()));
            require(static_cast<bool>(stop), "broker child owns stop event");
            return run_computer_broker(argv[2],
                                       [&] { return WaitForSingleObject(stop.get(), 0) != WAIT_TIMEOUT; });
        }
        if (argc == 2 && std::string_view(argv[1]) == "broker-native") {
            BrokerFixture broker;
            native_checks();
            rejects([&] { computer_broker_call("../invalid", "windows", Json::object(), {}); },
                    "COMPUTER_BROKER_CONFIG");
            rejects([&] { computer_broker_call(broker.pipe, "execute", Json::object(), {}); },
                    "COMPUTER_BROKER_PROTOCOL");
            broker.finish();
            rejects([&] { ComputerUse().windows(Json::object(), {}); }, "COMPUTER_BROKER_UNAVAILABLE");
            const auto other_image =
                fs::temp_directory_path() / path_from_utf8("devbox-peer-test-" + uuid() + ".exe");
            fs::copy_file(executable_path(), other_image);
            ScopeExit cleanup_image([&] { fs::remove(other_image); });
            {
                BrokerFixture other(other_image);
                rejects([&] { ComputerUse().windows(Json::object(), {}); }, "COMPUTER_BROKER_IDENTITY");
                other.finish();
            }
            std::cout << "native broker cross-process input, protocol rejection and disconnect passed\n";
            return 0;
        }
#endif
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
