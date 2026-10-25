#include "devbox/management.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#ifdef _WIN32
#include <sddl.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#endif
namespace devbox {
namespace {
std::string xml(std::string value) {
    value = replace_all(std::move(value), "&", "&amp;");
    value = replace_all(std::move(value), "<", "&lt;");
    value = replace_all(std::move(value), ">", "&gt;");
    value = replace_all(std::move(value), "\"", "&quot;");
    return replace_all(std::move(value), "'", "&apos;");
}
std::string unit_quote(std::string value, bool command = true) {
    value = replace_all(std::move(value), "\\", "\\\\");
    value = replace_all(std::move(value), "\"", "\\\"");
    value = replace_all(std::move(value), "%", "%%");
    if (command)
        value = replace_all(std::move(value), "$", "$$");
    return "\"" + value + "\"";
}
} // namespace
ServiceMemoryPlan service_memory_plan(std::string_view membership, std::string_view unit,
                                      std::uint64_t physical_bytes, std::uint64_t total_bytes) {
    if (!unit.starts_with("devbox-") || !unit.ends_with(".service") || unit.size() > 128 ||
        unit.find("..") != unit.npos || !std::all_of(unit.begin(), unit.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                   c == '_' || c == '.';
        }))
        throw Error("SERVICE_MEMORY_UNIT_INVALID");
    if (physical_bytes < 16 * 1024 * 1024 || physical_bytes > (1ULL << 40) || total_bytes < physical_bytes ||
        total_bytes > (16ULL << 40))
        throw Error("SERVICE_MEMORY_BOUNDS_INVALID");
    if (membership.empty() || membership.size() > 16384)
        throw Error("SERVICE_CGROUP_MEMBERSHIP_INVALID");
    std::optional<std::string> legacy, unified;
    for (const auto& line : split(membership, '\n')) {
        if (line.empty())
            continue;
        const auto first = line.find(':'),
                   second = first == line.npos ? line.npos : line.find(':', first + 1);
        if (first == line.npos || second == line.npos || first == 0 ||
            !std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(first),
                         [](char c) { return c >= '0' && c <= '9'; }))
            throw Error("SERVICE_CGROUP_MEMBERSHIP_INVALID");
        const auto controllers = line.substr(first + 1, second - first - 1);
        const auto group = line.substr(second + 1);
        if (first == 1 && line[0] == '0' && controllers.empty()) {
            if (unified)
                throw Error("SERVICE_CGROUP_MEMBERSHIP_DUPLICATE");
            unified = group;
        }
        for (const auto& controller : split(controllers, ','))
            if (controller == "memory") {
                if (legacy)
                    throw Error("SERVICE_CGROUP_MEMBERSHIP_DUPLICATE");
                legacy = group;
            }
    }
    const auto selected = legacy ? legacy : unified;
    const auto expected = "/system.slice/" + std::string(unit);
    if (!selected || *selected != expected)
        throw Error("SERVICE_MEMORY_REQUIRES_OWN_SYSTEM_UNIT");
    return {!legacy.has_value(), expected, physical_bytes, total_bytes};
}
Json apply_service_memory_guard(std::string_view unit, std::uint64_t physical_bytes,
                                std::uint64_t total_bytes) {
#if defined(__linux__) && !defined(__ANDROID__)
    if (::geteuid() != 0)
        throw Error("SERVICE_MEMORY_REQUIRES_ROOT_STARTUP");
    const auto read_descriptor = [](int descriptor, std::size_t limit) {
        std::string value;
        std::array<char, 1024> buffer{};
        for (;;) {
            const auto count = ::read(descriptor, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                throw Error("SERVICE_CGROUP_READ_FAILED");
            if (!count)
                break;
            if (static_cast<std::size_t>(count) > limit - value.size())
                throw Error("SERVICE_CGROUP_READ_LIMIT");
            value.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return value;
    };
    NativeHandle membership(::open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct statfs proc_type{};
    if (!membership || ::fstatfs(membership.get(), &proc_type) != 0 || proc_type.f_type != PROC_SUPER_MAGIC)
        throw Error("SERVICE_CGROUP_PROC_REQUIRED");
    const auto plan =
        service_memory_plan(read_descriptor(membership.get(), 16384), unit, physical_bytes, total_bytes);
    const auto page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0 || physical_bytes % static_cast<std::uint64_t>(page) ||
        total_bytes % static_cast<std::uint64_t>(page))
        throw Error("SERVICE_MEMORY_PAGE_ALIGNMENT");
    const auto expected_fs = plan.unified ? CGROUP2_SUPER_MAGIC : CGROUP_SUPER_MAGIC;
    const auto verify_directory = [&](int descriptor) {
        struct stat state{};
        struct statfs type{};
        if (descriptor < 0 || ::fstat(descriptor, &state) != 0 || !S_ISDIR(state.st_mode) ||
            state.st_uid != 0 || (state.st_mode & 022) || ::fstatfs(descriptor, &type) != 0 ||
            type.f_type != expected_fs)
            throw Error("SERVICE_CGROUP_PRIVATE_KERNEL_DIRECTORY_REQUIRED");
    };
    NativeHandle directory(::open(plan.unified ? "/sys/fs/cgroup" : "/sys/fs/cgroup/memory",
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    verify_directory(directory.get());
    for (const auto& component : {std::string("system.slice"), std::string(unit)}) {
        NativeHandle next(
            ::openat(directory.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        verify_directory(next.get());
        directory = std::move(next);
    }
    const auto open_control = [&](const char* name, int mode) {
        NativeHandle file(::openat(directory.get(), name, mode | O_CLOEXEC | O_NOFOLLOW));
        if (!file)
            throw Error("SERVICE_CGROUP_CONTROL_OPEN_FAILED: " + std::string(name) +
                        " errno=" + std::to_string(errno));
        struct stat state{};
        struct statfs type{};
        if (::fstat(file.get(), &state) != 0 || !S_ISREG(state.st_mode) || state.st_uid != 0 ||
            (state.st_mode & 022) || ::fstatfs(file.get(), &type) != 0 || type.f_type != expected_fs)
            throw Error("SERVICE_CGROUP_KERNEL_CONTROL_REQUIRED");
        return file;
    };
    const auto number = [](std::string_view text) {
        const auto value = trim(text);
        if (value == "max")
            return std::numeric_limits<std::uint64_t>::max();
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            throw Error("SERVICE_CGROUP_NUMBER_INVALID");
        return result;
    };
    const auto read_number = [&](const char* name) {
        auto file = open_control(name, O_RDONLY);
        return number(read_descriptor(file.get(), 64));
    };
    auto processes = open_control("cgroup.procs", O_RDONLY);
    const auto process_list = split(trim(read_descriptor(processes.get(), 65536)), '\n');
    const auto self = static_cast<std::uint64_t>(::getpid());
    if (std::none_of(process_list.begin(), process_list.end(),
                     [&](const auto& value) { return number(value) == self; }))
        throw Error("SERVICE_MEMORY_MEMBERSHIP_CHANGED");
    const bool alone = process_list.size() == 1;
    const auto physical = read_number(plan.unified ? "memory.max" : "memory.limit_in_bytes");
    if (!physical || physical > physical_bytes)
        throw Error("SERVICE_MEMORY_PHYSICAL_LIMIT_NOT_ENFORCED");
    const auto* control = plan.unified ? "memory.swap.max" : "memory.memsw.limit_in_bytes";
    const auto before = read_number(control);
    const auto requested = plan.unified ? total_bytes - physical : total_bytes;
    const auto target = std::min(before, requested);
    if (target != before) {
        // A supervisor recovery may have its verified former children alive.
        // Re-check an already enforced bound without mutating their cgroup;
        // only a fresh, otherwise empty group permits a limit change.
        if (!alone)
            throw Error("SERVICE_MEMORY_REQUIRES_EMPTY_STARTUP_CGROUP");
        auto file = open_control(control, O_WRONLY);
        const auto value = std::to_string(target) + "\n";
        ssize_t written;
        do {
            written = ::write(file.get(), value.data(), value.size());
        } while (written < 0 && errno == EINTR);
        if (written != static_cast<ssize_t>(value.size()))
            throw Error("SERVICE_MEMORY_LIMIT_WRITE_FAILED");
    }
    const auto after = read_number(control);
    if (after > target)
        throw Error("SERVICE_MEMORY_LIMIT_VERIFY_FAILED");
    return {{"verified", true},
            {"unit", unit},
            {"cgroup_version", plan.unified ? 2 : 1},
            {"physical_limit_bytes", physical},
            {"combined_limit_bytes", plan.unified ? physical + after : after}};
#else
    (void)unit;
    (void)physical_bytes;
    (void)total_bytes;
    throw Error("SERVICE_MEMORY_REQUIRES_LINUX");
#endif
}
std::string native_service_definition(std::string_view kind, const fs::path& binary, const fs::path& root) {
    const auto file = path_text(binary), directory = path_text(root);
    if (!binary.is_absolute() || !root.is_absolute() || file.find_first_of("\r\n") != file.npos ||
        directory.find_first_of("\r\n") != directory.npos)
        throw Error("Native service paths must be absolute and contain no newlines");
    const auto label = "devbox-native-" + sha256(directory).substr(0, 16);
    if (kind == "systemd")
        return "[Unit]\nDescription=Devbox native C++23 supervisor\nAfter=network-online.target\n\n"
               "[Service]\nType=simple\nWorkingDirectory=" +
               unit_quote(directory, false) + "\nExecStart=" + unit_quote(file) + " manage run --root " +
               unit_quote(directory) +
               "\nRestart=on-failure\nRestartSec=5\nTimeoutStopSec=180\nKillMode=process\nSendSIGKILL=no\n"
               "UMask=0077\n\n[Install]\nWantedBy=default.target\n";
    if (kind == "launchd")
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\"><dict><key>Label</key><string>" +
               label + "</string><key>ProgramArguments</key><array><string>" + xml(file) +
               "</string><string>manage</string><string>run</string><string>--root</string><string>" +
               xml(directory) + "</string></array><key>WorkingDirectory</key><string>" + xml(directory) +
               "</string><key>RunAtLoad</key><true/><key>KeepAlive</key><dict><key>SuccessfulExit</"
               "key><false/>"
               "</dict><key>ThrottleInterval</key><integer>5</integer><key>ExitTimeOut</key><integer>180</"
               "integer>"
               "<key>StandardOutPath</key><string>" +
               xml(path_text(root / "run" / "native" / "service.stdout.log")) +
               "</string><key>StandardErrorPath</key><string>" +
               xml(path_text(root / "run" / "native" / "service.stderr.log")) + "</string></dict></plist>\n";
    if (kind == "windows") {
        std::string sid;
#ifdef _WIN32
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            throw Error(windows_error());
        NativeHandle token(raw);
        DWORD bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> data(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, data.data(), bytes, &bytes))
            throw Error(windows_error());
        LPWSTR text = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text))
            throw Error(windows_error());
        ScopeExit release([&] { LocalFree(text); });
        sid = narrow(text);
#else
        throw Error("Windows service definitions require the actual Windows account SID");
#endif
        const auto arguments = "manage run --root " + quote_windows_argument(directory);
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
               "<RegistrationInfo><Description>Devbox native C++23 "
               "supervisor</Description></RegistrationInfo>"
               "<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" +
               xml(sid) + "</UserId></LogonTrigger></Triggers><Principals><Principal id=\"Author\"><UserId>" +
               xml(sid) +
               "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></"
               "Principal></Principals>"
               "<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
               "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</"
               "StopIfGoingOnBatteries>"
               "<AllowHardTerminate>false</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>"
               "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><RestartOnFailure><Interval>PT1M</"
               "Interval><Count>3</Count>"
               "</RestartOnFailure></Settings><Actions Context=\"Author\"><Exec><Command>" +
               xml(file) + "</Command><Arguments>" + xml(arguments) + "</Arguments><WorkingDirectory>" +
               xml(directory) + "</WorkingDirectory></Exec></Actions></Task>\n";
    }
    throw Error("Native service kind must be systemd, launchd or windows");
}
} // namespace devbox
