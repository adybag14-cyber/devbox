#include "devbox/capture.hpp"
#ifdef _WIN32
#include "devbox/native.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <dwmapi.h>
#include <map>
#include <set>
#include <tlhelp32.h>
#include <wincodec.h>
namespace devbox {
namespace {
struct Rect {
    int left, top, width, height;
};
struct Frame {
    unsigned width, height;
    std::vector<std::uint8_t> rgb;
};
struct Analysis {
    double mean, range, black, interior_mean, interior_black;
    bool suspicious;
};
template <class T> struct Com {
    T* ptr = nullptr;
    ~Com() {
        if (ptr)
            ptr->Release();
    }
    T** out() {
        return &ptr;
    }
    T* operator->() const {
        return ptr;
    }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
};
void check(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw Error(std::string(operation) + ": " + windows_error(static_cast<DWORD>(result)));
}
std::size_t validate_dimensions(int width, int height) {
    if (width <= 0 || height <= 0)
        throw Error("capture dimensions must be positive.");
    if (width > 32768 || height > 32768)
        throw Error("capture dimensions exceed the maximum supported dimension 32768.");
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<unsigned>(height);
    if (pixels > 64 * 1024 * 1024)
        throw Error("capture dimensions exceed the maximum supported pixel count 67108864.");
    return static_cast<std::size_t>(pixels);
}
Rect virtual_screen() {
    Rect rect{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
              GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (rect.width <= 0 || rect.height <= 0)
        throw Error("Windows reported an empty virtual display.");
    return rect;
}
std::optional<Rect> intersect(Rect a, Rect b) {
    const auto left = std::max(a.left, b.left), top = std::max(a.top, b.top);
    const auto right =
        std::min(static_cast<std::int64_t>(a.left) + a.width, static_cast<std::int64_t>(b.left) + b.width);
    const auto bottom =
        std::min(static_cast<std::int64_t>(a.top) + a.height, static_cast<std::int64_t>(b.top) + b.height);
    if (right <= left || bottom <= top)
        return {};
    return Rect{left, top, static_cast<int>(right - left), static_cast<int>(bottom - top)};
}
std::optional<Frame> copy_frame(Rect rect, HWND window = nullptr, UINT flags = 0) {
    const auto pixels = validate_dimensions(rect.width, rect.height);
    const auto screen = GetDC(nullptr);
    if (!screen)
        throw Error("GetDC failed for the Windows virtual desktop.");
    ScopeExit release_screen([&] { ReleaseDC(nullptr, screen); });
    const auto memory = CreateCompatibleDC(screen);
    if (!memory)
        throw Error("CreateCompatibleDC failed during Windows capture.");
    ScopeExit delete_memory([&] { DeleteDC(memory); });
    const auto bitmap = CreateCompatibleBitmap(screen, rect.width, rect.height);
    if (!bitmap)
        throw Error("CreateCompatibleBitmap failed during Windows capture.");
    ScopeExit delete_bitmap([&] { DeleteObject(bitmap); });
    const auto old = SelectObject(memory, bitmap);
    if (!old || old == HGDI_ERROR)
        throw Error("SelectObject failed while preparing desktop capture.");
    ScopeExit restore([&] { SelectObject(memory, old); });
    BOOL copied;
    if (window) {
        PatBlt(memory, 0, 0, rect.width, rect.height, BLACKNESS);
        copied = PrintWindow(window, memory, flags);
    } else
        copied =
            BitBlt(memory, 0, 0, rect.width, rect.height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, old);
    restore.disarm();
    if (!copied) {
        if (window)
            return {};
        throw Error("BitBlt failed while copying the Windows desktop compositor.");
    }
    std::vector<std::uint8_t> bgra(pixels * 4);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.width;
    info.bmiHeader.biHeight = -rect.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto lines =
        GetDIBits(memory, bitmap, 0, static_cast<UINT>(rect.height), bgra.data(), &info, DIB_RGB_COLORS);
    if (lines != rect.height)
        throw Error("GetDIBits returned " + std::to_string(lines) + " scanlines for a " +
                    std::to_string(rect.height) + "-line capture.");
    Frame frame{static_cast<unsigned>(rect.width), static_cast<unsigned>(rect.height),
                std::vector<std::uint8_t>(pixels * 3)};
    for (std::size_t i = 0; i < pixels; ++i) {
        frame.rgb[i * 3] = bgra[i * 4 + 2];
        frame.rgb[i * 3 + 1] = bgra[i * 4 + 1];
        frame.rgb[i * 3 + 2] = bgra[i * 4];
    }
    return frame;
}
Analysis analyze(const Frame& frame) {
    const auto columns = std::min(32u, frame.width), rows = std::min(32u, frame.height);
    unsigned count = 0, black = 0, interior_count = 0, interior_black = 0;
    double sum = 0, interior_sum = 0, minimum = 255, maximum = 0;
    for (unsigned row = 0; row < rows; ++row) {
        const double ny = (row + 0.5) / rows;
        const auto y = (static_cast<std::uint64_t>(row) * 2 + 1) * frame.height / (2 * rows);
        for (unsigned col = 0; col < columns; ++col) {
            const double nx = (col + 0.5) / columns;
            const auto x = (static_cast<std::uint64_t>(col) * 2 + 1) * frame.width / (2 * columns);
            const auto offset = static_cast<std::size_t>(y * frame.width + x) * 3;
            const auto r = frame.rgb[offset], g = frame.rgb[offset + 1], b = frame.rgb[offset + 2];
            const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            const bool dark = r <= 8 && g <= 8 && b <= 8;
            sum += luma;
            minimum = std::min(minimum, luma);
            maximum = std::max(maximum, luma);
            black += dark;
            ++count;
            if (nx >= 0.08 && nx <= 0.92 && ny >= 0.18 && ny <= 0.92) {
                interior_sum += luma;
                interior_black += dark;
                ++interior_count;
            }
        }
    }
    const auto mean = sum / count, ratio = static_cast<double>(black) / count;
    const auto imean = interior_count ? interior_sum / interior_count : mean;
    const auto iratio = interior_count ? static_cast<double>(interior_black) / interior_count : ratio;
    return {mean,   maximum - minimum,
            ratio,  imean,
            iratio, (ratio >= 0.985 && mean <= 12) || (iratio >= 0.94 && imean <= 16)};
}
std::vector<std::uint8_t> encode_jpeg(const Frame& frame, unsigned quality) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        check(initialized, "initialize WIC COM");
    ScopeExit uninitialize([&] {
        if (SUCCEEDED(initialized))
            CoUninitialize();
    });
    Com<IWICImagingFactory> factory;
    check(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.out())),
        "create WIC factory");
    Com<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, stream.out()), "create JPEG memory stream");
    Com<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.out()), "create JPEG encoder");
    check(encoder->Initialize(stream.ptr, WICBitmapEncoderNoCache), "initialize JPEG encoder");
    Com<IWICBitmapFrameEncode> encoded;
    Com<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(encoded.out(), properties.out()), "create JPEG frame");
    PROPBAG2 property{};
    property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
    VARIANT value{};
    value.vt = VT_R4;
    value.fltVal = static_cast<float>(quality) / 100.0f;
    check(properties->Write(1, &property, &value), "set JPEG quality");
    check(encoded->Initialize(properties.ptr), "initialize JPEG frame");
    check(encoded->SetSize(frame.width, frame.height), "set JPEG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    check(encoded->SetPixelFormat(&format), "set JPEG pixel format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
        throw Error("WIC JPEG encoder rejected 24-bit BGR pixels");
    auto bgr = frame.rgb;
    for (std::size_t i = 0; i < bgr.size(); i += 3)
        std::swap(bgr[i], bgr[i + 2]);
    check(encoded->WritePixels(frame.height, frame.width * 3, static_cast<UINT>(bgr.size()), bgr.data()),
          "encode Windows capture JPEG");
    check(encoded->Commit(), "commit JPEG frame");
    check(encoder->Commit(), "commit JPEG encoder");
    STATSTG stat{};
    check(stream->Stat(&stat, STATFLAG_NONAME), "read JPEG stream size");
    if (stat.cbSize.QuadPart > 64 * 1024 * 1024)
        throw Error("JPEG output exceeds 64 MiB capture limit");
    check(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr), "rewind JPEG stream");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG received = 0;
    check(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &received), "read encoded JPEG");
    if (received != bytes.size())
        throw Error("JPEG stream was incomplete");
    validate_capture_image(bytes, "image/jpeg");
    return bytes;
}
struct Process {
    std::uint32_t parent;
    std::string name;
};
std::map<std::uint32_t, Process> processes() {
    NativeHandle handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!handle)
        throw Error("CreateToolhelp32Snapshot failed during capture process discovery.");
    std::map<std::uint32_t, Process> result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(handle.get(), &entry))
        throw Error("Process32FirstW failed during capture process discovery.");
    do {
        auto name = narrow(entry.szExeFile);
        if (name.ends_with(".exe"))
            name.resize(name.size() - 4);
        result.emplace(entry.th32ProcessID, Process{entry.th32ParentProcessID, std::move(name)});
    } while (Process32NextW(handle.get(), &entry));
    return result;
}
std::optional<std::string> process_name(std::uint32_t pid) {
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process)
        return {};
    std::wstring value(32768, L'\0');
    DWORD length = static_cast<DWORD>(value.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, value.data(), &length))
        return {};
    value.resize(length);
    return path_text(fs::path(value).stem());
}
struct Window {
    HWND handle;
    DWORD owner;
    Rect rect;
    std::string title;
};
struct Selection {
    const std::set<std::uint32_t>* pids;
    std::optional<Window> best;
    std::exception_ptr error;
};
BOOL CALLBACK select_window(HWND handle, LPARAM param) {
    auto& selection = *reinterpret_cast<Selection*>(param);
    try {
        if (!IsWindowVisible(handle) || IsIconic(handle))
            return TRUE;
        DWORD owner = 0;
        GetWindowThreadProcessId(handle, &owner);
        if (!selection.pids->contains(owner))
            return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            return TRUE;
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
            !GetWindowRect(handle, &bounds))
            return TRUE;
        const auto width = static_cast<std::int64_t>(bounds.right) - bounds.left;
        const auto height = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
        if (width < 32 || height < 32 || width > INT_MAX || height > INT_MAX)
            return TRUE;
        if (selection.best && width * height <= static_cast<std::int64_t>(selection.best->rect.width) *
                                                    selection.best->rect.height)
            return TRUE;
        std::wstring title(static_cast<std::size_t>(std::clamp(GetWindowTextLengthW(handle), 0, 32767)) + 1,
                           L'\0');
        const auto length = GetWindowTextW(handle, title.data(), static_cast<int>(title.size()));
        title.resize(static_cast<std::size_t>(std::max(0, length)));
        selection.best = Window{
            handle, owner, Rect{bounds.left, bounds.top, static_cast<int>(width), static_cast<int>(height)},
            narrow(title)};
        return TRUE;
    } catch (...) {
        selection.error = std::current_exception();
        return FALSE;
    }
}
double rounded(double value, double scale) {
    return std::round(value * scale) / scale;
}
} // namespace
ImageCapture native_capture(std::optional<std::uint32_t> pid, unsigned quality, bool include_tree,
                            const Cancel& cancel) {
    if (cancel)
        cancel->check();
    if (quality < 1 || quality > 100)
        throw Error("quality must be between 1 and 100.");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!pid) {
        const auto rect = virtual_screen();
        const auto frame = copy_frame(rect);
        return {encode_jpeg(*frame, quality), "image/jpeg",
                Json{{"capture_mode", "full_display"},
                     {"capture_method", "DesktopCompositorCopy"},
                     {"left", rect.left},
                     {"top", rect.top},
                     {"width", rect.width},
                     {"height", rect.height},
                     {"quality", quality}}};
    }
    if (*pid == 0)
        throw Error("pid must be a positive Windows process ID.");
    const auto table = processes();
    const auto root = table.find(*pid);
    if (root == table.end())
        throw Error("Windows process " + std::to_string(*pid) + " does not exist.");
    const auto name = process_name(*pid).value_or(root->second.name);
    std::set<std::uint32_t> candidates{*pid};
    if (include_tree) {
        std::deque<std::uint32_t> queue{*pid};
        while (!queue.empty()) {
            const auto parent = queue.front();
            queue.pop_front();
            for (const auto& [child, info] : table)
                if (info.parent == parent && candidates.insert(child).second)
                    queue.push_back(child);
        }
    }
    Selection selection{&candidates, {}, {}};
    EnumWindows(select_window, reinterpret_cast<LPARAM>(&selection));
    if (selection.error)
        std::rethrow_exception(selection.error);
    if (!selection.best)
        throw Error("Process " + std::to_string(*pid) + " (" + name +
                    ") and its visible child processes have no non-minimized, non-cloaked top-level window "
                    "to capture.");
    const auto window = *selection.best;
    std::optional<Frame> chosen;
    std::optional<Analysis> analysis;
    std::optional<UINT> last_flags;
    std::string method;
    bool rejected = false;
    for (const UINT flags : {2u, 0u}) {
        auto frame = copy_frame(window.rect, window.handle, flags);
        if (frame) {
            analysis = analyze(*frame);
            last_flags = flags;
            if (!analysis->suspicious) {
                chosen = std::move(frame);
                method = flags == 2 ? "PrintWindow(PW_RENDERFULLCONTENT)" : "PrintWindow(default)";
                break;
            }
            rejected = true;
        }
    }
    auto captured = window.rect;
    bool clipped = false;
    if (!chosen) {
        DwmFlush();
        const auto visible = intersect(window.rect, virtual_screen());
        if (!visible)
            throw Error("The window is completely outside the visible virtual desktop and PrintWindow did "
                        "not return usable pixels.");
        captured = *visible;
        clipped = captured.left != window.rect.left || captured.top != window.rect.top ||
                  captured.width != window.rect.width || captured.height != window.rect.height;
        chosen = copy_frame(captured);
        method = "DesktopCompositorCopy(window-bounds)";
    }
    Json meta{
        {"capture_mode", "program_pid"},
        {"pid", *pid},
        {"process_name", name},
        {"window_owner_pid", window.owner},
        {"process_tree_fallback", window.owner != *pid},
        {"candidate_pid_count", candidates.size()},
        {"window_handle", static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(window.handle))},
        {"window_title", window.title},
        {"capture_method", method},
        {"print_window_rejected", rejected},
        {"print_window_flags", last_flags ? Json(*last_flags) : Json()},
        {"print_window_mean_luma", analysis ? Json(rounded(analysis->mean, 1000)) : Json()},
        {"print_window_luma_range", analysis ? Json(rounded(analysis->range, 1000)) : Json()},
        {"print_window_near_black_ratio", analysis ? Json(rounded(analysis->black, 10000)) : Json()},
        {"print_window_interior_mean_luma", analysis ? Json(rounded(analysis->interior_mean, 1000)) : Json()},
        {"print_window_interior_near_black_ratio",
         analysis ? Json(rounded(analysis->interior_black, 10000)) : Json()},
        {"requested_left", window.rect.left},
        {"requested_top", window.rect.top},
        {"requested_width", window.rect.width},
        {"requested_height", window.rect.height},
        {"left", captured.left},
        {"top", captured.top},
        {"width", captured.width},
        {"height", captured.height},
        {"clipped_to_display", clipped},
        {"screen_fallback_may_include_occluders", starts_with(method, "DesktopCompositorCopy")},
        {"quality", quality}};
    return {encode_jpeg(*chosen, quality), "image/jpeg", std::move(meta)};
}
} // namespace devbox
#endif
