#include "devbox/capture.hpp"
#ifndef _WIN32
#include "devbox/native.hpp"
#include <algorithm>
#include <charconv>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <libproc.h>
#endif
namespace devbox {
namespace {
ProcessOutput command(std::string_view program, std::vector<std::string> args, const Cancel& cancel) {
    ProcessOptions options;
    options.timeout = Millis(20000);
    options.max_capture_chars = 128000;
    return spawn_process(program, args, options, cancel);
}
struct Process {
    std::uint32_t parent;
    std::string name;
};
std::map<std::uint32_t, Process> process_table(const Cancel& cancel) {
    std::map<std::uint32_t, Process> result;
#ifdef __APPLE__
    const auto expected = proc_listallpids(nullptr, 0);
    if (expected < 0)
        throw Error("list macOS processes");
    std::vector<pid_t> pids(static_cast<std::size_t>(expected) + 1024);
    const auto count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int index = 0; index < std::min(count, static_cast<int>(pids.size())); ++index) {
        if (cancel)
            cancel->check();
        const auto pid = pids[static_cast<std::size_t>(index)];
        proc_bsdinfo info{};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
            continue;
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        const auto length = proc_pidpath(pid, path, sizeof(path));
        result.emplace(static_cast<std::uint32_t>(pid),
                       Process{info.pbi_ppid, length > 0 ? path_text(path_from_utf8(path).filename())
                                                         : std::string(info.pbi_comm)});
    }
#else
    for (const auto& entry : fs::directory_iterator("/proc")) {
        if (cancel)
            cancel->check();
        const auto name = path_text(entry.path().filename());
        std::uint32_t pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (parsed.ec != std::errc() || parsed.ptr != name.data() + name.size())
            continue;
        try {
            const auto stat = read_file(entry.path() / "stat", 65536);
            const auto open = stat.find('('), close = stat.rfind(')');
            if (open == stat.npos || close == stat.npos || close <= open)
                continue;
            std::istringstream tail(stat.substr(close + 1));
            char state;
            std::uint32_t parent;
            if (tail >> state >> parent)
                result.emplace(pid, Process{parent, stat.substr(open + 1, close - open - 1)});
        } catch (const std::exception&) {
        }
    }
#endif
    return result;
}
std::set<std::uint32_t> process_tree(const std::map<std::uint32_t, Process>& table, std::uint32_t root,
                                     bool include_tree) {
    std::set<std::uint32_t> pids{root};
    if (include_tree) {
        std::deque<std::uint32_t> queue{root};
        while (!queue.empty()) {
            const auto parent = queue.front();
            queue.pop_front();
            for (const auto& [child, info] : table)
                if (info.parent == parent && pids.insert(child).second)
                    queue.push_back(child);
        }
    }
    return pids;
}
struct Window {
    std::string id;
    std::uint32_t owner;
    int left, top;
    unsigned width, height;
};
#ifdef __APPLE__
std::optional<Window> find_window(const std::set<std::uint32_t>& pids, const Cancel& cancel) {
    const auto list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list)
        throw Error("CoreGraphics could not enumerate on-screen windows.");
    ScopeExit release([&] { CFRelease(list); });
    std::optional<Window> best;
    const auto integer = [](CFDictionaryRef item, CFStringRef key) {
        std::int64_t result = 0;
        const auto value = CFDictionaryGetValue(item, key);
        if (value && CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result);
        return result;
    };
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
        if (cancel)
            cancel->check();
        const auto item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        const auto owner = static_cast<std::uint32_t>(integer(item, kCGWindowOwnerPID));
        if (!pids.contains(owner))
            continue;
        const auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(item, kCGWindowBounds));
        CGRect rect{};
        if (!bounds || !CGRectMakeWithDictionaryRepresentation(bounds, &rect) || rect.size.width < 32 ||
            rect.size.height < 32)
            continue;
        if (rect.size.width > 32768 || rect.size.height > 32768)
            continue;
        Window value{std::to_string(integer(item, kCGWindowNumber)),
                     owner,
                     static_cast<int>(rect.origin.x),
                     static_cast<int>(rect.origin.y),
                     static_cast<unsigned>(rect.size.width),
                     static_cast<unsigned>(rect.size.height)};
        if (!best || static_cast<std::uint64_t>(value.width) * value.height >
                         static_cast<std::uint64_t>(best->width) * best->height)
            best = value;
    }
    return best;
}
#else
std::int64_t geometry_value(const std::string& text, std::string_view label) {
    for (const auto& line : split(text, '\n')) {
        auto value = trim(line);
        if (!starts_with(value, label))
            continue;
        value = trim(value.substr(label.size()));
        std::int64_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec == std::errc() && parsed.ptr == value.data() + value.size())
            return number;
        throw Error("invalid xwininfo geometry: " + std::string(label));
    }
    throw Error("xwininfo output omitted " + std::string(label));
}
std::optional<Window> find_window(const std::set<std::uint32_t>& pids, const Cancel& cancel) {
    std::vector<std::pair<std::string, std::uint32_t>> ids;
    if (find_program("xdotool")) {
        for (const auto pid : pids) {
            try {
                const auto output =
                    command("xdotool", {"search", "--onlyvisible", "--pid", std::to_string(pid)}, cancel);
                for (const auto& line : split(output.stdout_text, '\n'))
                    if (!trim(line).empty())
                        ids.emplace_back(trim(line), pid);
            } catch (const ProcessError& e) {
                if (e.exit_code != 1)
                    throw;
            }
        }
    } else if (find_program("wmctrl")) {
        const auto output = command("wmctrl", {"-lp"}, cancel);
        for (const auto& line : split(output.stdout_text, '\n')) {
            std::istringstream fields(line);
            std::string id, desktop;
            std::uint32_t pid;
            if (fields >> id >> desktop >> pid && pids.contains(pid))
                ids.emplace_back(id, pid);
        }
    } else
        throw Error("Window discovery requires xdotool or wmctrl on Linux.");
    std::optional<Window> best;
    for (const auto& [id, owner] : ids) {
        if (cancel)
            cancel->check();
        if (!find_program("xwininfo"))
            throw Error("Window geometry discovery requires xwininfo on Linux. Install xwininfo.");
        try {
            const auto text = command("xwininfo", {"-id", id}, cancel).stdout_text;
            const auto left = geometry_value(text, "Absolute upper-left X:"),
                       top = geometry_value(text, "Absolute upper-left Y:");
            const auto width = geometry_value(text, "Width:"), height = geometry_value(text, "Height:");
            if (width < 32 || height < 32 || width > UINT32_MAX || height > UINT32_MAX || left < INT32_MIN ||
                left > INT32_MAX || top < INT32_MIN || top > INT32_MAX)
                continue;
            Window value{id,
                         owner,
                         static_cast<int>(left),
                         static_cast<int>(top),
                         static_cast<unsigned>(width),
                         static_cast<unsigned>(height)};
            if (!best || static_cast<std::uint64_t>(value.width) * value.height >
                             static_cast<std::uint64_t>(best->width) * best->height)
                best = value;
        } catch (const std::exception&) {
            if (cancel && cancel->cancelled())
                throw;
        }
    }
    return best;
}
#endif
} // namespace
ImageCapture native_capture(std::optional<std::uint32_t> pid, unsigned quality, bool include_tree,
                            const Cancel& cancel) {
    if (cancel)
        cancel->check();
    if (quality < 1 || quality > 100)
        throw Error("quality must be between 1 and 100.");
    if (pid && *pid == 0)
        throw Error("pid must be a positive process ID.");
#ifndef __APPLE__
    if (!environment("DISPLAY") && !environment("WAYLAND_DISPLAY"))
        throw Error("No DISPLAY or WAYLAND_DISPLAY is available for Linux screen capture.");
#endif
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-native-capture-" + uuid());
    if (::mkdir(root.c_str(), 0700) != 0)
        throw Error("create private capture temporary directory");
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    const auto path = root / "capture.png";
    Json metadata;
    if (pid) {
        const auto table = process_table(cancel);
        const auto found = table.find(*pid);
        if (found == table.end())
            throw Error(std::string(
#ifdef __APPLE__
                            "macOS"
#else
                            "Linux"
#endif
                            ) +
                        " process " + std::to_string(*pid) + " does not exist.");
        const auto pids = process_tree(table, *pid, include_tree);
        const auto window = find_window(pids, cancel);
#ifdef __APPLE__
        if (!window)
            throw Error("No visible Quartz window found for the requested process tree.");
        command("screencapture", {"-x", "-l", window->id, path_text(path)}, cancel);
        const auto method = "screencapture(window-id)";
#else
        if (!window)
            throw Error("Process " + std::to_string(*pid) + " (" + found->second.name +
                        ") and its visible child processes have no discoverable top-level X11 window.");
        if (!find_program("import"))
            throw Error("No supported Linux process-window screenshot tool is installed. Install ImageMagick "
                        "import.");
        command("import", {"-window", window->id, path_text(path)}, cancel);
        const auto method = "import";
#endif
        metadata = Json{{"capture_mode", "program_pid"},
                        {"capture_method", method},
                        {"pid", *pid},
                        {"process_name", found->second.name},
                        {"window_owner_pid", window->owner},
                        {"process_tree_fallback", window->owner != *pid},
                        {"candidate_pid_count", pids.size()},
                        {"window_id", window->id},
                        {"left", window->left},
                        {"top", window->top},
                        {"width", window->width},
                        {"height", window->height},
                        {"quality", quality}};
#ifdef __APPLE__
        metadata["window_id"] = std::stoull(window->id);
#endif
    } else {
        std::string tool;
        std::vector<std::string> args;
#ifdef __APPLE__
        tool = "screencapture";
        args = {"-x", path_text(path)};
#else
        if (environment("WAYLAND_DISPLAY") && find_program("grim")) {
            tool = "grim";
            args = {path_text(path)};
        } else if (find_program("gnome-screenshot")) {
            tool = "gnome-screenshot";
            args = {"-f", path_text(path)};
        } else if (find_program("scrot")) {
            tool = "scrot";
            args = {path_text(path)};
        } else if (find_program("import")) {
            tool = "import";
            args = {"-window", "root", path_text(path)};
        } else
            throw Error("No supported Linux screenshot tool is installed. Install grim, gnome-screenshot, "
                        "scrot, or ImageMagick import.");
#endif
        command(tool, args, cancel);
        metadata = Json{{"capture_mode", "full_display"}, {"capture_method", tool}, {"quality", quality}};
    }
    const auto bytes = read_file(path, 64 * 1024 * 1024);
    std::vector<std::uint8_t> image(bytes.begin(), bytes.end());
    validate_capture_image(image, "image/png");
    return {std::move(image), "image/png", std::move(metadata)};
}
} // namespace devbox
#endif
