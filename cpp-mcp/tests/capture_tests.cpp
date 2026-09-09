#include "devbox/capture.hpp"
#include "devbox/native.hpp"
#include <future>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw Error(message);
}
constexpr std::uint8_t png[]{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
int fixture_worker(const std::vector<std::string>& args) {
    const auto mode = env_or("DEVBOX_TEST_CAPTURE_MODE", "");
    if (mode.empty())
        return run_capture_worker(args);
    const auto root = path_from_utf8(env_or("DEVBOX_TEST_CAPTURE_ROOT", ""));
    write_file(root / "started", "1", true);
    if (mode == "stall")
        std::this_thread::sleep_for(Millis(30000));
    if (mode == "slow")
        std::this_thread::sleep_for(Millis(350));
    if (mode == "retry" && !fs::exists(root / "attempted")) {
        write_file(root / "attempted", "1");
        std::cerr << "BitBlt transient fixture failure\n";
        return 1;
    }
    write_file(path_from_utf8(args.at(0)),
               mode == "invalid" ? std::string("wrong image")
                                 : std::string(reinterpret_cast<const char*>(png), sizeof(png)));
    std::cout << Json{{"mime_type", "image/png"}, {"fixture", true}}.dump() << '\n';
    return 0;
}
std::future<ImageCapture> invoke(asio::io_context& io, CaptureService& service, const Cancel& cancel = {}) {
    return asio::co_spawn(io, service.capture({}, 70, true, cancel), asio::use_future);
}
#ifdef _WIN32
void paint(HWND window, HDC dc) {
    RECT rect{};
    GetClientRect(window, &rect);
    HBRUSH brush = CreateSolidBrush(RGB(40, 120, 230));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    rect.right /= 2;
    brush = CreateSolidBrush(RGB(230, 100, 45));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
LRESULT CALLBACK fixture_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        paint(window, dc);
        EndPaint(window, &ps);
        return 0;
    }
    if (message == WM_PRINT || message == WM_PRINTCLIENT) {
        paint(window, reinterpret_cast<HDC>(wparam));
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void native_window_test(asio::io_context& io, CaptureService& service) {
    std::promise<DWORD> ready;
    auto future = ready.get_future();
    std::thread gui([&] {
        WNDCLASSW cls{};
        cls.lpfnWndProc = fixture_proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"DevboxCppCaptureFixture";
        RegisterClassW(&cls);
        const auto small =
            CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, L"Devbox C++ small capture fixture",
                            WS_OVERLAPPEDWINDOW, 80, 80, 220, 160, nullptr, nullptr, cls.hInstance, nullptr);
        const auto big = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName,
                                         L"Devbox C++ largest capture fixture", WS_OVERLAPPEDWINDOW, 100, 100,
                                         480, 320, nullptr, nullptr, cls.hInstance, nullptr);
        const auto minimized = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName,
                                               L"Devbox C++ minimized fixture", WS_OVERLAPPEDWINDOW, 120, 120,
                                               680, 440, nullptr, nullptr, cls.hInstance, nullptr);
        if (small && big && minimized) {
            ShowWindow(small, SW_SHOWNOACTIVATE);
            ShowWindow(big, SW_SHOWNOACTIVATE);
            ShowWindow(minimized, SW_SHOWMINNOACTIVE);
            UpdateWindow(small);
            UpdateWindow(big);
        }
        ready.set_value(small && big && minimized ? GetCurrentThreadId() : 0);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        for (auto window : {small, big, minimized})
            if (window)
                DestroyWindow(window);
        UnregisterClassW(cls.lpszClassName, cls.hInstance);
    });
    const auto id = future.get();
    ScopeExit close([&] {
        if (id)
            PostThreadMessageW(id, WM_QUIT, 0, 0);
        gui.join();
    });
    require(id != 0, "create owned capture fixture windows");
    auto value = asio::co_spawn(io, service.capture(process_id(), 82, false, {}), asio::use_future).get();
    validate_capture_image(value.image, "image/jpeg");
    require(value.metadata["window_title"] == "Devbox C++ largest capture fixture", value.metadata.dump());
    require(value.metadata["window_owner_pid"] == process_id() &&
                value.metadata["process_tree_fallback"] == false,
            "capture stays on requested PID");
    require(value.metadata["quality"] == 82 && value.metadata["bytes"] == value.image.size() &&
                value.metadata["sha256"] == sha256(value.image),
            "JPEG quality and integrity metadata");
    require(value.metadata["print_window_mean_luma"].get<double>() > 20 && value.image.size() > 1000,
            "native capture has colored pixels");
    (void)native_capture(process_id(), 60, false);
    const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 5; ++i)
        (void)native_capture(process_id(), 60, false);
    const auto after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    require(after <= before, "native capture releases GDI objects after repeated attempts");
    std::cout << "native-window: " << value.metadata.dump() << '\n';
}
#endif
} // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--capture-worker") {
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i)
            args.emplace_back(argv[i]);
        return fixture_worker(args);
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-capture-test-" + uuid());
    fs::create_directories(root);
    ScopeExit cleanup([&] {
        set_environment("DEVBOX_TEST_CAPTURE_MODE", {});
        set_environment("DEVBOX_TEST_CAPTURE_ROOT", {});
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
#ifdef _WIN32
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
        auto config = std::make_shared<Config>();
        config->screen_capture_attempt_timeout_ms = 2500;
        config->screen_capture_queue_timeout_ms = 100;
        config->screen_capture_retries = 1;
        asio::io_context io;
        auto guard = asio::make_work_guard(io);
        std::thread worker([&] { io.run(); });
        ScopeExit stop([&] {
            guard.reset();
            io.stop();
            worker.join();
        });
        CaptureService service(config);
        set_environment("DEVBOX_TEST_CAPTURE_ROOT", path_text(root));
        set_environment("DEVBOX_TEST_CAPTURE_MODE", "retry");
        auto retried = invoke(io, service).get();
        require(retried.metadata["capture_attempts"] == 2 && retried.metadata["capture_retried"] == true,
                "retry transient capture exactly once");
        set_environment("DEVBOX_TEST_CAPTURE_MODE", "invalid");
        try {
            (void)invoke(io, service).get();
            throw Error("invalid image accepted");
        } catch (const std::exception& e) {
            require(std::string(e.what()).find("failed after 2 attempt") != std::string::npos, e.what());
        }
        set_environment("DEVBOX_TEST_CAPTURE_MODE", "slow");
        fs::remove(root / "started");
        auto first = invoke(io, service);
        const auto limit = Clock::now() + Millis(2000);
        while (!fs::exists(root / "started") && Clock::now() < limit)
            std::this_thread::sleep_for(Millis(5));
        require(fs::exists(root / "started"), "first worker started");
        try {
            (void)invoke(io, service).get();
            throw Error("busy queue accepted");
        } catch (const std::exception& e) {
            require(std::string(e.what()).find("queue remained busy for 100 ms") != std::string::npos,
                    e.what());
        }
        (void)first.get();
        set_environment("DEVBOX_TEST_CAPTURE_MODE", "stall");
        auto cancel = std::make_shared<Cancellation>();
        auto stalled = invoke(io, service, cancel);
        std::this_thread::sleep_for(Millis(100));
        const auto start = Clock::now();
        cancel->cancel();
        try {
            (void)stalled.get();
            throw Error("cancel ignored");
        } catch (const ProcessError& e) {
            require(e.aborted, "worker cancellation preserves process error");
        }
        require(Clock::now() - start < Millis(4000), "capture worker cancellation is bounded");
        config->screen_capture_attempt_timeout_ms = 120;
        config->screen_capture_retries = 0;
        try {
            (void)invoke(io, service).get();
            throw Error("timeout ignored");
        } catch (const ProcessError& e) {
            require(e.timed_out, "capture worker timeout preserved");
        }
        config->screen_capture_attempt_timeout_ms = 10000;
        set_environment("DEVBOX_TEST_CAPTURE_MODE", {});
#ifdef _WIN32
        native_window_test(io, service);
#elif !defined(__APPLE__)
        const auto display = environment("DISPLAY"), wayland = environment("WAYLAND_DISPLAY");
        set_environment("DISPLAY", {});
        set_environment("WAYLAND_DISPLAY", {});
        ScopeExit restore([&] {
            set_environment("DISPLAY", display);
            set_environment("WAYLAND_DISPLAY", wayland);
        });
        try {
            (void)native_capture({}, 70, true);
            throw Error("headless capture accepted");
        } catch (const std::exception& e) {
            require(std::string(e.what()).find("No DISPLAY or WAYLAND_DISPLAY") != std::string::npos,
                    e.what());
        }
#endif
        std::cout << "capture tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
