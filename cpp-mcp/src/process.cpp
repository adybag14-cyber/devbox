#include "devbox/process.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
extern char** environ;
#endif

namespace devbox {
namespace {
std::atomic<std::uint64_t> probe_count{0}, probe_total_ns{0}, probe_max_ns{0};
struct ProcessProbeTiming {
    Clock::time_point started = Clock::now();
    ~ProcessProbeTiming() {
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
        probe_count.fetch_add(1, std::memory_order_relaxed);
        probe_total_ns.fetch_add(ns, std::memory_order_relaxed);
        auto prior = probe_max_ns.load(std::memory_order_relaxed);
        while (prior < ns && !probe_max_ns.compare_exchange_weak(prior, ns, std::memory_order_relaxed)) {
        }
    }
};
std::vector<std::string> characters(std::string_view text) {
    std::vector<std::string> values;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        values.emplace_back(text.substr(i, n));
        i += n;
    }
    return values;
}
std::size_t scalar_count(std::string_view value) {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xc0) != 0x80; }));
}
std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Millis>(Clock::now() - start).count());
}
void check_string(std::string_view value) {
    if (value.find('\0') != std::string_view::npos)
        throw Error("Process arguments cannot contain NUL bytes.");
}
} // namespace
void CaptureAccumulator::push_text(std::string_view text) {
    for (auto&& scalar : characters(text)) {
        ++count_;
        if (!limit_ || (!truncated_ && count_ <= *limit_)) {
            head_.push_back(std::move(scalar));
            continue;
        }
        truncated_ = true;
        const auto head_limit = *limit_ / 2;
        const auto tail_limit = *limit_ - head_limit;
        while (head_.size() > head_limit) {
            tail_.push_front(std::move(head_.back()));
            head_.pop_back();
        }
        tail_.push_back(std::move(scalar));
        while (tail_.size() > tail_limit)
            tail_.pop_front();
    }
}
void CaptureAccumulator::push(std::string_view bytes) {
    pending_.append(bytes);
    auto boundary = pending_.size();
    if (!pending_.empty()) {
        auto lead = pending_.size() - 1;
        while (lead > 0 && (static_cast<unsigned char>(pending_[lead]) & 0xc0) == 0x80)
            --lead;
        const auto c = static_cast<unsigned char>(pending_[lead]);
        const std::size_t needed = c >= 0xc2 && c <= 0xdf   ? 2
                                   : c >= 0xe0 && c <= 0xef ? 3
                                   : c >= 0xf0 && c <= 0xf4 ? 4
                                                            : 1;
        bool valid_partial = pending_.size() - lead < needed;
        if (valid_partial && lead + 1 < pending_.size()) {
            const auto second = static_cast<unsigned char>(pending_[lead + 1]);
            if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
                (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90))
                valid_partial = false;
        }
        if (valid_partial)
            boundary = lead;
    }
    push_text(sanitize_utf8(std::string_view(pending_).substr(0, boundary)));
    pending_.erase(0, boundary);
}
void CaptureAccumulator::finish() {
    push_text(sanitize_utf8(pending_));
    pending_.clear();
}
CaptureResult CaptureAccumulator::snapshot() const {
    CaptureResult result;
    result.original_chars = count_;
    result.truncated = truncated_;
    if (truncated_ && limit_ == 0)
        return result;
    for (const auto& part : head_)
        result.text += part;
    if (truncated_) {
        result.text += "\n... middle capture omitted " +
                       std::to_string(count_ - head_.size() - tail_.size()) + " characters ...\n";
        for (const auto& part : tail_)
            result.text += part;
    }
    return result;
}
CaptureResult read_text_file_bounded(const fs::path& path, std::optional<std::size_t> limit) {
    CaptureAccumulator capture(limit);
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 16384> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        capture.push(std::string_view(buffer.data(), static_cast<std::size_t>(stream.gcount())));
    }
    capture.finish();
    return capture.snapshot();
}
std::string summarize_process_failure(std::string_view file, int code, std::string_view stdout_text,
                                      std::string_view stderr_text) {
    const auto message = trim(stderr_text);
    if (!message.empty()) {
        if (scalar_count(message) <= 4096)
            return message;
        const std::string suffix = "\n... error summary truncated to 4096 characters ...";
        std::string result;
        auto parts = characters(message);
        for (std::size_t i = 0; i < 4096 - suffix.size(); ++i)
            result += parts[i];
        return result + suffix;
    }
    const auto count = scalar_count(stdout_text);
    return std::string(file) + " exited with code " + std::to_string(code) +
           (count ? " after producing " + std::to_string(count) +
                        " characters of stdout; see the bounded stdout field."
                  : ".");
}
std::string quote_windows_argument(std::string_view value) {
    check_string(value);
    if (!value.empty() && value.find_first_of(" \t\n\v\"") == std::string_view::npos)
        return std::string(value);
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const auto c : value) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        out.append(backslashes * (c == '"' ? 2 : 1), '\\');
        backslashes = 0;
        if (c == '"')
            out += '\\';
        out += c;
    }
    out.append(backslashes * 2, '\\');
    return out + '"';
}
Environment current_environment() {
    Environment result;
#ifdef _WIN32
    auto block = GetEnvironmentStringsW();
    if (!block)
        throw Error(windows_error());
    ScopeExit release([&] { FreeEnvironmentStringsW(block); });
    for (auto entry = block; *entry; entry += wcslen(entry) + 1) {
        const auto line = narrow(entry);
        const auto equal = line.find('=', line.starts_with('=') ? 1 : 0);
        if (equal != std::string::npos)
            result[line.substr(0, equal)] = line.substr(equal + 1);
    }
#else
    for (auto item = environ; item && *item; ++item) {
        const std::string line(*item);
        const auto equal = line.find('=');
        if (equal != std::string::npos)
            result[line.substr(0, equal)] = line.substr(equal + 1);
    }
#endif
    return result;
}
std::optional<fs::path> find_program(std::string_view program, const Environment* env) {
    if (program.empty())
        return std::nullopt;
    check_string(program);
    const auto supplied = path_from_utf8(program);
    if (supplied.has_parent_path()) {
        std::error_code ec;
        return fs::is_regular_file(supplied, ec) ? std::optional(fs::absolute(supplied)) : std::nullopt;
    }
    const auto read_env = [&](std::string_view name, std::string fallback = {}) {
        if (!env)
            return env_or(name, fallback);
        for (const auto& [key, value] : *env) {
#ifdef _WIN32
            if (lower(key) == lower(std::string(name)))
                return value;
#else
            if (key == name)
                return value;
#endif
        }
        return fallback;
    };
#ifdef _WIN32
    const auto dirs = split(read_env("PATH"), ';', false);
    const auto extensions = supplied.has_extension()
                                ? std::vector<std::string>{""}
                                : split(read_env("PATHEXT", ".COM;.EXE;.BAT;.CMD"), ';', false);
#else
    const auto dirs = split(read_env("PATH", "/usr/local/bin:/usr/bin:/bin"), ':', true);
    const std::vector<std::string> extensions{""};
#endif
    for (auto directory : dirs) {
        if (directory.size() > 1 && directory.front() == '"' && directory.back() == '"')
            directory = directory.substr(1, directory.size() - 2);
        for (const auto& extension : extensions) {
            auto candidate =
                path_from_utf8(directory) / path_from_utf8(std::string(program) + trim(extension));
            std::error_code ec;
            if (!fs::is_regular_file(candidate, ec))
                continue;
#ifndef _WIN32
            if (::access(candidate.c_str(), X_OK) != 0)
                continue;
#else
            // PATHEXT is commonly uppercase; report the spelling owned by the filesystem.
            const auto length = GetLongPathNameW(candidate.c_str(), nullptr, 0);
            if (length) {
                std::wstring actual(length, L'\0');
                const auto written = GetLongPathNameW(candidate.c_str(), actual.data(), length);
                if (written && written < length) {
                    actual.resize(written);
                    candidate = fs::path(actual);
                }
            }
#endif
            return fs::absolute(candidate);
        }
    }
    return std::nullopt;
}
#ifdef _WIN32
std::string windows_error(unsigned long code) {
    wchar_t* buffer = nullptr;
    const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string message =
        length ? trim(narrow(std::wstring_view(buffer, length))) : "Win32 error " + std::to_string(code);
    if (buffer)
        LocalFree(buffer);
    return message + " (os error " + std::to_string(code) + ")";
}
namespace {
std::optional<std::uint64_t> handle_instance(HANDLE process) {
    FILETIME created{}, ended{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &ended, &kernel, &user))
        return std::nullopt;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
} // namespace
bool process_alive(std::uint32_t pid) {
    ProcessProbeTiming timing;
    if (!pid)
        return false;
    NativeHandle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
}
std::optional<std::uint64_t> process_instance(std::uint32_t pid) {
    if (!pid)
        return std::nullopt;
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return process ? handle_instance(process.get()) : std::nullopt;
}
bool is_administrator() {
    NativeHandle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        throw Error(windows_error());
    token.reset(raw);
    TOKEN_ELEVATION elevation{};
    DWORD length = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &length))
        throw Error(windows_error());
    return elevation.TokenIsElevated != 0;
}
bool terminate_process_tree(std::uint32_t pid, std::optional<std::uint64_t> expected) {
    if (pid <= 4 || pid == process_id() || !expected)
        return false;
    NativeHandle root(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
    if (!root || handle_instance(root.get()) != expected)
        return false;
    struct Entry {
        DWORD pid, parent;
        std::optional<std::uint64_t> instance;
    };
    std::vector<Entry> entries;
    NativeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot) {
        PROCESSENTRY32W item{};
        item.dwSize = sizeof(item);
        if (Process32FirstW(snapshot.get(), &item))
            do {
                entries.push_back(
                    {item.th32ProcessID, item.th32ParentProcessID, process_instance(item.th32ProcessID)});
            } while (Process32NextW(snapshot.get(), &item));
    }
    std::vector<Entry> owned{{pid, 0, expected}};
    for (std::size_t i = 0; i < owned.size(); ++i) {
        const auto parent = owned[i];
        for (const auto& entry : entries) {
            if (entry.parent == parent.pid && entry.instance && parent.instance &&
                *entry.instance >= *parent.instance && entry.pid != process_id() &&
                std::none_of(owned.begin(), owned.end(), [&](const auto& v) { return v.pid == entry.pid; }))
                owned.push_back(entry);
        }
    }
    for (auto item = owned.rbegin(); item != owned.rend(); ++item) {
        NativeHandle child(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, item->pid));
        if (child && handle_instance(child.get()) == item->instance)
            TerminateProcess(child.get(), 1);
    }
    return WaitForSingleObject(root.get(), 2000) == WAIT_OBJECT_0;
}
#else
bool process_alive(std::uint32_t pid) {
    ProcessProbeTiming timing;
    if (pid == 0 || pid > static_cast<std::uint32_t>(INT32_MAX))
        return false;
#ifdef __linux__
    try {
        const auto status = read_file(path_from_utf8("/proc/" + std::to_string(pid) + "/stat"), 8192);
        const auto close = status.rfind(')');
        if (close != std::string::npos && close + 2 < status.size() &&
            (status[close + 2] == 'Z' || status[close + 2] == 'X'))
            return false;
    } catch (...) {
    }
#endif
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}
std::optional<std::uint64_t> process_instance(std::uint32_t pid) {
    if (!pid)
        return std::nullopt;
    try {
#if defined(__linux__)
        const auto stat = read_file(path_from_utf8("/proc/" + std::to_string(pid) + "/stat"), 8192);
        const auto close = stat.rfind(')');
        if (close == std::string::npos)
            return std::nullopt;
        std::istringstream fields(stat.substr(close + 1));
        std::string token;
        for (int i = 0; i < 20; ++i)
            if (!(fields >> token))
                return std::nullopt;
        static const auto boot = trim(read_file("/proc/sys/kernel/random/boot_id", 256));
        const auto digest = sha256(boot + ":" + token);
        std::uint64_t result = 0;
        for (unsigned i = 0; i < 8; ++i)
            result |= std::stoull(digest.substr(i * 2, 2), nullptr, 16) << (i * 8);
        return result;
#elif defined(__APPLE__)
        proc_bsdinfo info{};
        if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
            return std::nullopt;
        return info.pbi_start_tvsec
                   ? std::optional<std::uint64_t>(info.pbi_start_tvsec * 1000000 +
                                                  std::min<std::uint64_t>(info.pbi_start_tvusec, 999999))
                   : std::nullopt;
#else
        return std::nullopt;
#endif
    } catch (...) {
        return std::nullopt;
    }
}
bool is_administrator() {
    return ::geteuid() == 0;
}
bool terminate_process_tree(std::uint32_t pid, std::optional<std::uint64_t> expected) {
    if (pid <= 1 || pid == process_id() || !expected || process_instance(pid) != expected)
        return false;
    const auto native = static_cast<pid_t>(pid);
    const auto target = ::getpgid(native) == native && ::getpgrp() != native ? -native : native;
    if (::kill(target, SIGTERM) != 0 && errno != ESRCH)
        return false;
    const auto deadline = Clock::now() + Millis(250);
    while (Clock::now() < deadline && process_matches_instance(pid, expected))
        std::this_thread::sleep_for(Millis(10));
    if (process_matches_instance(pid, expected))
        ::kill(target, SIGKILL);
    return !process_matches_instance(pid, expected);
}
#endif
bool process_matches_instance(std::uint32_t pid, std::optional<std::uint64_t> expected) {
    if (!pid)
        return false;
    const auto observed = process_instance(pid);
    if (expected)
        return observed == expected && process_alive(pid);
    return process_alive(pid);
}

// Platform backends share classification, capture and callback semantics.
struct RawProcessResult {
    std::optional<int> code, signal;
    std::uint32_t pid = 0;
    bool timed_out = false, aborted = false;
};
#ifdef _WIN32
namespace {
struct Pipe {
    NativeHandle parent, child;
};
Pipe output_pipe() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &security, 65536))
        throw Error(windows_error());
    Pipe pipe{NativeHandle(read), NativeHandle(write)};
    if (!SetHandleInformation(pipe.parent.get(), HANDLE_FLAG_INHERIT, 0))
        throw Error(windows_error());
    return pipe;
}
Pipe input_pipe() {
    const auto name = wide("\\\\.\\pipe\\devbox-cpp-stdin-" + uuid());
    NativeHandle server(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    if (!server)
        throw Error(windows_error());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    NativeHandle child(
        CreateFileW(name.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!child)
        throw Error(windows_error());
    // The client has already connected synchronously. ERROR_PIPE_CONNECTED is expected.
    OVERLAPPED connect{};
    NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        throw Error(windows_error());
    connect.hEvent = event.get();
    if (!ConnectNamedPipe(server.get(), &connect)) {
        const auto error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            CancelIoEx(server.get(), &connect);
            DWORD transferred = 0;
            if (error == ERROR_IO_PENDING)
                GetOverlappedResult(server.get(), &connect, &transferred, TRUE);
            throw Error(windows_error(error));
        }
    }
    return {std::move(server), std::move(child)};
}
void read_available(NativeHandle& pipe, CaptureAccumulator& capture, OutputStream stream,
                    const ProcessOptions& options) {
    std::array<char, 16384> buffer{};
    // Bound each pass so continuously chatty children cannot starve cancellation or stderr.
    for (int pass = 0; pipe && pass < 16; ++pass) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                pipe.reset();
                return;
            }
            throw Error(windows_error());
        }
        if (!available)
            return;
        DWORD count = 0;
        if (!ReadFile(pipe.get(), buffer.data(),
                      std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                pipe.reset();
                return;
            }
            throw Error(windows_error());
        }
        const std::string_view bytes(buffer.data(), count);
        capture.push(bytes);
        if (options.on_output)
            options.on_output(stream, bytes);
    }
}
RawProcessResult run_native(std::string_view file, const std::vector<std::string>& args,
                            const ProcessOptions& options, const Cancel& cancel, CaptureAccumulator& out,
                            CaptureAccumulator& err) {
    auto stdout_pipe = output_pipe(), stderr_pipe = output_pipe(), stdin_pipe = input_pipe();
    const auto resolved = find_program(file, options.env ? &*options.env : nullptr);
    std::string program = resolved ? path_text(*resolved) : std::string(file);
    std::string command = quote_windows_argument(program);
    if (options.windows_raw_arguments)
        command += " " + *options.windows_raw_arguments;
    else
        for (const auto& argument : args)
            command += " " + quote_windows_argument(argument);
    const auto extension = lower(path_text(path_from_utf8(program).extension()));
    if (extension == ".cmd" || extension == ".bat") {
        // CRT quoting is not CMD quoting. Use a script adapter in RuntimeExecutor for batch files.
        throw Error("Batch programs require the Windows shell adapter.");
    }
    auto native_command = wide(command);
    if (native_command.size() >= 32767)
        throw Error("Windows process command line exceeds 32766 UTF-16 units.");
    auto native_program = wide(program);
    std::vector<wchar_t> environment_block;
    if (options.env) {
        std::vector<std::pair<std::wstring, std::wstring>> entries;
        for (const auto& [key, value] : *options.env) {
            if (key.empty() || key.find('=', key.starts_with('=') ? 1 : 0) != std::string::npos)
                throw Error("Invalid child environment key.");
            check_string(key);
            check_string(value);
            entries.emplace_back(wide(key), wide(value));
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
        });
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i && _wcsicmp(entries[i - 1].first.c_str(), entries[i].first.c_str()) == 0)
                throw Error("Duplicate case-insensitive child environment key.");
            const auto entry = entries[i].first + L"=" + entries[i].second;
            environment_block.insert(environment_block.end(), entry.begin(), entry.end());
            environment_block.push_back(L'\0');
        }
        environment_block.push_back(L'\0');
        if (entries.empty())
            environment_block.push_back(L'\0');
    }
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attribute_storage(attribute_bytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
        throw Error(windows_error());
    ScopeExit free_attributes([&] { DeleteProcThreadAttributeList(attributes); });
    HANDLE handles[]{stdin_pipe.child.get(), stdout_pipe.child.get(), stderr_pipe.child.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles),
                                   nullptr, nullptr))
        throw Error(windows_error());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information{};
    const auto cwd = options.cwd ? options.cwd->wstring() : std::wstring();
    NativeHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
        throw Error(windows_error());
    if (cancel)
        cancel->check();
    if (!CreateProcessW(native_program.c_str(), native_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED |
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        options.env ? environment_block.data() : nullptr, options.cwd ? cwd.c_str() : nullptr,
                        &startup.StartupInfo, &information))
        throw Error(windows_error());
    NativeHandle process(information.hProcess), thread(information.hThread);
    bool assigned = false;
    ScopeExit terminate_on_error([&] {
        if (assigned)
            TerminateJobObject(job.get(), 1);
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 3000);
    });
    if (!AssignProcessToJobObject(job.get(), process.get()))
        throw Error("Unable to contain child process: " + windows_error());
    assigned = true;
    if (options.on_pid)
        options.on_pid(information.dwProcessId);
    const auto started = Clock::now();
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        throw Error(windows_error());
    thread.reset();
    stdout_pipe.child.reset();
    stderr_pipe.child.reset();
    stdin_pipe.child.reset();
    NativeHandle write_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!write_event)
        throw Error(windows_error());
    OVERLAPPED writer{};
    writer.hEvent = write_event.get();
    bool write_pending = false;
    std::size_t input_offset = 0;
    const std::string_view input = options.input ? std::string_view(*options.input) : std::string_view();
    ScopeExit cancel_writer([&] {
        if (write_pending && stdin_pipe.parent) {
            CancelIoEx(stdin_pipe.parent.get(), &writer);
            DWORD count = 0;
            GetOverlappedResult(stdin_pipe.parent.get(), &writer, &count, TRUE);
        }
    });
    RawProcessResult result;
    result.pid = information.dwProcessId;
    std::optional<Clock::time_point> exited, forced;
    while (true) {
        read_available(stdout_pipe.parent, out, OutputStream::stdout_stream, options);
        read_available(stderr_pipe.parent, err, OutputStream::stderr_stream, options);
        if (stdin_pipe.parent) {
            DWORD written = 0;
            if (write_pending && GetOverlappedResult(stdin_pipe.parent.get(), &writer, &written, FALSE)) {
                write_pending = false;
                input_offset += written;
            } else if (write_pending && GetLastError() != ERROR_IO_INCOMPLETE) {
                write_pending = false;
                stdin_pipe.parent.reset();
            }
            if (!write_pending && stdin_pipe.parent) {
                if (input_offset >= input.size())
                    stdin_pipe.parent.reset();
                else {
                    ResetEvent(writer.hEvent);
                    const auto count =
                        static_cast<DWORD>(std::min<std::size_t>(65536, input.size() - input_offset));
                    if (WriteFile(stdin_pipe.parent.get(), input.data() + input_offset, count, &written,
                                  &writer))
                        input_offset += written;
                    else if (GetLastError() == ERROR_IO_PENDING)
                        write_pending = true;
                    else
                        stdin_pipe.parent.reset();
                }
            }
        }
        const auto now = Clock::now();
        if (!exited && WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (GetExitCodeProcess(process.get(), &code))
                result.code = static_cast<int>(code);
            exited = now;
        }
        if (!exited && !forced &&
            ((cancel && cancel->cancelled()) || (options.timeout && now - started >= *options.timeout))) {
            result.aborted = cancel && cancel->cancelled();
            result.timed_out = !result.aborted;
            TerminateJobObject(job.get(), 1);
            forced = now;
        }
        if (exited && ((!stdout_pipe.parent && !stderr_pipe.parent) || now - *exited >= Millis(1000)))
            break;
        if (forced && !exited && now - *forced >= options.termination_grace)
            break;
        if (cancel && !cancel->cancelled())
            cancel->wait_for(Millis(5));
        else
            std::this_thread::sleep_for(Millis(5));
    }
    if (!exited) {
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.get(), 3000);
    }
    terminate_on_error.disarm();
    return result;
}
} // namespace
#else
namespace {
struct Pipe {
    NativeHandle read, write;
};
Pipe make_pipe() {
    int handles[2];
#if defined(__linux__)
    if (::pipe2(handles, O_CLOEXEC) != 0)
        throw Error(std::strerror(errno));
#else
    if (::pipe(handles) != 0)
        throw Error(std::strerror(errno));
    for (const auto fd : handles)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    return {NativeHandle(handles[0]), NativeHandle(handles[1])};
}
void nonblocking(int fd) {
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw Error(std::strerror(errno));
}
void read_available(NativeHandle& pipe, CaptureAccumulator& capture, OutputStream stream,
                    const ProcessOptions& options) {
    std::array<char, 16384> buffer{};
    for (int pass = 0; pipe && pass < 16; ++pass) {
        const auto count = ::read(pipe.get(), buffer.data(), buffer.size());
        if (!count) {
            pipe.reset();
            return;
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            throw Error(std::strerror(errno));
        }
        const std::string_view bytes(buffer.data(), static_cast<std::size_t>(count));
        capture.push(bytes);
        if (options.on_output)
            options.on_output(stream, bytes);
    }
}
RawProcessResult run_native(std::string_view file, const std::vector<std::string>& args,
                            const ProcessOptions& options, const Cancel& cancel, CaptureAccumulator& out,
                            CaptureAccumulator& err) {
    auto input_pipe = make_pipe(), stdout_pipe = make_pipe(), stderr_pipe = make_pipe();
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
        throw Error(std::strerror(error));
    ScopeExit release_actions([&] { posix_spawn_file_actions_destroy(&actions); });
    const auto checked = [](int result) {
        if (result)
            throw Error(std::strerror(result));
    };
    checked(posix_spawn_file_actions_adddup2(&actions, input_pipe.read.get(), STDIN_FILENO));
    checked(posix_spawn_file_actions_adddup2(&actions, stdout_pipe.write.get(), STDOUT_FILENO));
    checked(posix_spawn_file_actions_adddup2(&actions, stderr_pipe.write.get(), STDERR_FILENO));
    for (const auto fd : {input_pipe.read.get(), input_pipe.write.get(), stdout_pipe.read.get(),
                          stdout_pipe.write.get(), stderr_pipe.read.get(), stderr_pipe.write.get()})
        if (fd > STDERR_FILENO)
            checked(posix_spawn_file_actions_addclose(&actions, fd));
    if (options.cwd)
        checked(posix_spawn_file_actions_addchdir_np(&actions, options.cwd->c_str()));
    posix_spawnattr_t attributes;
    checked(posix_spawnattr_init(&attributes));
    ScopeExit release_attributes([&] { posix_spawnattr_destroy(&attributes); });
    checked(posix_spawnattr_setflags(&attributes,
                                     POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    checked(posix_spawnattr_setpgroup(&attributes, 0));
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    checked(posix_spawnattr_setsigmask(&attributes, &empty));
    checked(posix_spawnattr_setsigdefault(&attributes, &defaults));
    std::vector<std::string> arguments{std::string(file)};
    arguments.insert(arguments.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : arguments)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment_strings;
    std::vector<char*> envp;
    if (options.env) {
        for (const auto& [key, value] : *options.env) {
            check_string(key);
            check_string(value);
            if (key.empty() || key.find('=') != std::string::npos)
                throw Error("Invalid child environment key.");
            environment_strings.push_back(key + "=" + value);
        }
        for (auto& entry : environment_strings)
            envp.push_back(entry.data());
        envp.push_back(nullptr);
    }
    const auto resolved = find_program(file, options.env ? &*options.env : nullptr);
    const auto program = resolved ? path_text(*resolved) : std::string(file);
    if (cancel)
        cancel->check();
    pid_t child = 0;
    const auto started = Clock::now();
    checked(posix_spawn(&child, program.c_str(), &actions, &attributes, argv.data(),
                        options.env ? envp.data() : environ));
    bool reaped = false;
    ScopeExit terminate_on_error([&] {
        if (!reaped) {
            ::kill(-child, SIGKILL);
            int status;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }
    });
    input_pipe.read.reset();
    stdout_pipe.write.reset();
    stderr_pipe.write.reset();
    nonblocking(input_pipe.write.get());
    nonblocking(stdout_pipe.read.get());
    nonblocking(stderr_pipe.read.get());
    if (options.on_pid)
        options.on_pid(static_cast<std::uint32_t>(child));
    // Block SIGPIPE only on this worker; consume our write failure before restoring the mask.
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    checked(pthread_sigmask(SIG_BLOCK, &blocked, &previous));
    sigpending(&pending);
    const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
    ScopeExit restore_mask([&] {
        if (!already_pending) {
#ifdef __APPLE__
            sigset_t pending_now;
            if (sigpending(&pending_now) == 0 && sigismember(&pending_now, SIGPIPE) == 1) {
                int received = 0;
                sigwait(&blocked, &received);
            }
#else
            timespec zero{};
            while (sigtimedwait(&blocked, nullptr, &zero) >= 0) {
            }
#endif
        }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    });
    RawProcessResult result;
    result.pid = static_cast<std::uint32_t>(child);
    std::optional<Clock::time_point> exited, forced;
    bool killed = false;
    std::size_t input_offset = 0;
    const std::string_view input = options.input ? std::string_view(*options.input) : std::string_view();
    while (true) {
        read_available(stdout_pipe.read, out, OutputStream::stdout_stream, options);
        read_available(stderr_pipe.read, err, OutputStream::stderr_stream, options);
        if (input_pipe.write) {
            if (input_offset == input.size())
                input_pipe.write.reset();
            else {
                const auto count = ::write(input_pipe.write.get(), input.data() + input_offset,
                                           std::min<std::size_t>(65536, input.size() - input_offset));
                if (count > 0)
                    input_offset += static_cast<std::size_t>(count);
                else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                    input_pipe.write.reset();
            }
        }
        const auto now = Clock::now();
        int status = 0;
        if (!exited) {
            const auto waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) {
                reaped = true;
                exited = now;
                if (WIFEXITED(status))
                    result.code = WEXITSTATUS(status);
                if (WIFSIGNALED(status))
                    result.signal = WTERMSIG(status);
            } else if (waited < 0 && errno != EINTR)
                throw Error(std::strerror(errno));
        }
        if (!exited && !forced &&
            ((cancel && cancel->cancelled()) || (options.timeout && now - started >= *options.timeout))) {
            result.aborted = cancel && cancel->cancelled();
            result.timed_out = !result.aborted;
            ::kill(-child, SIGTERM);
            forced = now;
        }
        if (forced && !killed && now - *forced >= Millis(250)) {
            ::kill(-child, SIGKILL);
            killed = true;
        }
        if (exited && ((!stdout_pipe.read && !stderr_pipe.read) || now - *exited >= Millis(1000))) {
            if (forced && !killed)
                ::kill(-child, SIGKILL);
            break;
        }
        if (forced && !exited && now - *forced >= options.termination_grace + Millis(250))
            break;
        pollfd descriptors[]{{stdout_pipe.read.get(), POLLIN, 0},
                             {stderr_pipe.read.get(), POLLIN, 0},
                             {input_pipe.write.get(), POLLOUT, 0}};
        ::poll(descriptors, 3, 10);
    }
    if (reaped)
        terminate_on_error.disarm();
    return result;
}
} // namespace
#endif
ProcessOutput spawn_process(std::string_view file, const std::vector<std::string>& args,
                            const ProcessOptions& options, const Cancel& cancel) {
    const auto started = Clock::now();
    CaptureAccumulator out(options.max_capture_chars), err(options.max_capture_chars);
    RawProcessResult raw;
    std::string launch_failure;
    try {
        check_string(file);
        for (const auto& arg : args)
            check_string(arg);
        if (options.windows_raw_arguments)
            check_string(*options.windows_raw_arguments);
        raw = run_native(file, args, options, cancel, out, err);
    } catch (const Cancelled& error) {
        raw.aborted = true;
        launch_failure = error.what();
    } catch (const std::exception& error) {
        launch_failure = error.what();
    }
    out.finish();
    err.finish();
    const auto stdout_capture = out.snapshot(), stderr_capture = err.snapshot();
    if (!launch_failure.empty() || raw.aborted || raw.timed_out || raw.code != 0) {
        const auto message =
            !launch_failure.empty() ? launch_failure
            : raw.aborted           ? "Command cancelled by the MCP client."
            : raw.timed_out ? "Command timed out after " + std::to_string(options.timeout->count()) + " ms."
            : raw.code || raw.signal ? summarize_process_failure(file, raw.code.value_or(-1),
                                                                 stdout_capture.text, stderr_capture.text)
                                     : "Command process disappeared without an exit status.";
        ProcessError error(message);
        error.exit_code = raw.code;
        error.signal = raw.signal;
        error.stdout_text = stdout_capture.text;
        error.stderr_text = stderr_capture.text;
        error.file = file;
        error.args = args;
        error.aborted = raw.aborted;
        error.timed_out = raw.timed_out;
        error.elapsed_ms = elapsed(started);
        throw error;
    }
    return {stdout_capture.text,
            stderr_capture.text,
            stdout_capture.original_chars,
            stderr_capture.original_chars,
            stdout_capture.truncated,
            stderr_capture.truncated,
            *raw.code,
            raw.pid,
            elapsed(started)};
}
Json process_probe_metrics() {
    const auto count = probe_count.load(std::memory_order_relaxed),
               total = probe_total_ns.load(std::memory_order_relaxed),
               maximum = probe_max_ns.load(std::memory_order_relaxed);
    return Json{{"count", count},
                {"averageMs", static_cast<double>(count ? total / count : 0) / 1000000.0},
                {"maxMs", static_cast<double>(maximum) / 1000000.0},
                {"backend",
#ifdef _WIN32
                 "win32-openprocess"
#else
                 "posix-process-identity"
#endif
                }};
}
} // namespace devbox
