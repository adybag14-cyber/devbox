#include "devbox/native.hpp"
#include "devbox/telemetry.hpp"
#include <algorithm>
#include <sstream>
#ifdef _WIN32
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#else
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#endif
namespace devbox {
namespace {
#ifdef _WIN32
std::optional<std::uint32_t> current_process_thread_count() {
    // Query the documented process records directly. Walking every thread in a
    // Toolhelp snapshot was the dominant engine startup cost on busy hosts.
    // Resolve dynamically and retain Toolhelp as the compatibility fallback.
    using Query = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query)
        return std::nullopt;
    constexpr auto length_mismatch = static_cast<NTSTATUS>(0xc0000004UL);
    constexpr std::size_t maximum = 64 * 1024 * 1024;
    std::vector<unsigned char> data(256 * 1024);
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG length = 0;
        const auto status =
            query(SystemProcessInformation, data.data(), static_cast<ULONG>(data.size()), &length);
        if (status == length_mismatch) {
            const auto next = std::max(data.size() * 2, static_cast<std::size_t>(length) + 65536);
            if (next > maximum)
                return std::nullopt;
            data.resize(next);
            continue;
        }
        if (status < 0 || length > data.size())
            return std::nullopt;
        std::size_t offset = 0;
        while (offset <= length && length - offset >= sizeof(SYSTEM_PROCESS_INFORMATION)) {
            const auto* record = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(data.data() + offset);
            if (reinterpret_cast<std::uintptr_t>(record->UniqueProcessId) == process_id())
                return record->NumberOfThreads ? std::optional<std::uint32_t>(record->NumberOfThreads)
                                               : std::nullopt;
            const auto next = record->NextEntryOffset;
            if (next < sizeof(SYSTEM_PROCESS_INFORMATION) || next > length - offset ||
                next % alignof(SYSTEM_PROCESS_INFORMATION) != 0)
                break;
            offset += next;
        }
        return std::nullopt;
    }
    return std::nullopt;
}
#endif
struct ProcessMetrics {
    std::uint64_t rss = 0, private_bytes = 0;
    Json cpu = nullptr, platform = nullptr;
};
ProcessMetrics process_metrics() {
    ProcessMetrics result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        result.rss = memory.WorkingSetSize;
        result.private_bytes = memory.PagefileUsage;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        result.cpu = (ticks(kernel) + ticks(user)) / 10000;
    }
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    static std::mutex cache_mutex;
    static Clock::time_point sampled{};
    static std::uint32_t threads = 0;
    std::lock_guard lock(cache_mutex);
    if (Clock::now() - sampled >= std::chrono::seconds(60)) {
        if (const auto count = current_process_thread_count())
            threads = *count;
        else {
            NativeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            threads = 0;
            if (snapshot && Thread32First(snapshot.get(), &entry)) {
                do {
                    if (entry.th32OwnerProcessID == process_id())
                        ++threads;
                } while (Thread32Next(snapshot.get(), &entry));
            }
        }
        sampled = Clock::now();
    }
    result.platform = Json{{"cpuTotalMs", result.cpu}, {"handles", handles}, {"threads", threads}};
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        result.cpu = (std::uint64_t(usage.ru_utime.tv_sec) + std::uint64_t(usage.ru_stime.tv_sec)) * 1000 +
                     std::uint64_t(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000;
#if defined(__linux__)
    try {
        std::istringstream lines(read_file("/proc/self/status", 65536));
        std::string line;
        while (std::getline(lines, line))
            if (line.starts_with("VmRSS:")) {
                std::istringstream value(line.substr(6));
                value >> result.rss;
                result.rss *= 1024;
                break;
            }
    } catch (...) {
    }
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        result.rss = info.resident_size;
#endif
#endif
    return result;
}
Json pointer_or_null(const Json& value, const char* pointer) {
    const Json::json_pointer key(pointer);
    return value.contains(key) ? value[key] : Json();
}
} // namespace
PerformanceMonitor::PerformanceMonitor(const Config& config, BackgroundTasks& background,
                                       std::function<Json()> build)
    : background_(background), state_path_(config.mcp_performance_state_path),
      history_(state_path_.parent_path() / "mcp-performance-history.jsonl", 4 * 1024 * 1024, 3),
      build_(std::move(build)) {
    cache_ = capture();
}
void PerformanceMonitor::attach(asio::any_io_executor executor, Cancel cancel) {
    if (attached_.exchange(true))
        throw Error("Performance monitor is already attached");
    background_.mark_started("performance-sampler", "periodic");
    asio::co_spawn(asio::make_strand(std::move(executor)), sample(std::move(cancel)),
                   [this](std::exception_ptr error) {
                       if (error) {
                           try {
                               std::rethrow_exception(error);
                           } catch (const Cancelled&) {
                           } catch (const std::exception& e) {
                               background_.mark_stopped("performance-sampler", e.what());
                               return;
                           }
                       }
                       background_.mark_stopped("performance-sampler");
                   });
    background_.once("performance-initial-persistence", Millis(0), [this](const Cancel&) { persist(); });
    background_.periodic("performance-persistence", Millis(5000), Millis(10000),
                         [this](const Cancel&) { persist(); });
}
asio::awaitable<void> PerformanceMonitor::sample(Cancel cancel) {
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    auto expected = Clock::now() + Millis(20), drift = Clock::now() + Millis(1000);
    while (!cancel->cancelled()) {
        timer.expires_at(expected);
        co_await timer.async_wait(asio::use_awaitable);
        if (cancel->cancelled())
            break;
        const auto now = Clock::now();
        auto late = [](auto a, auto b) {
            return std::max(0.0, std::chrono::duration<double, std::milli>(a - b).count());
        };
        {
            std::lock_guard lock(mutex_);
            delays_.push_back({now, late(now, expected)});
            while (delays_.size() > 15000 ||
                   (!delays_.empty() && now - delays_.front().at > std::chrono::minutes(5)))
                delays_.pop_front();
            if (now >= drift) {
                drifts_.push_back({now, late(now, drift)});
                drift = now + Millis(1000);
                while (!drifts_.empty() && now - drifts_.front().at > std::chrono::minutes(5))
                    drifts_.pop_front();
                background_.attempt("performance-sampler");
                background_.success("performance-sampler");
            }
        }
        expected += Millis(20);
        if (now - expected > Millis(1000))
            expected = now + Millis(20);
    }
}
Json PerformanceMonitor::capture() {
    std::deque<Sample> delays, drifts;
    {
        std::lock_guard lock(mutex_);
        delays = delays_;
        drifts = drifts_;
    }
    const auto now = Clock::now();
    auto window = [&](std::uint64_t seconds) {
        const auto cutoff = now - std::chrono::seconds(seconds);
        std::vector<double> values;
        double drift = 0;
        for (const auto& sample : delays)
            if (sample.at >= cutoff)
                values.push_back(sample.ms);
        for (const auto& sample : drifts)
            if (sample.at >= cutoff)
                drift = std::max(drift, sample.ms);
        std::sort(values.begin(), values.end());
        auto percentile = [&](std::size_t p) {
            return values.empty() ? 0.0 : values[((values.size() - 1) * p + 50) / 100];
        };
        return Json{{"windowSeconds", seconds}, {"sampleCount", values.size()},
                    {"p50Ms", percentile(50)},  {"p95Ms", percentile(95)},
                    {"p99Ms", percentile(99)},  {"maxMs", values.empty() ? 0.0 : values.back()},
                    {"timerDriftMaxMs", drift}};
    };
    auto short_window = window(10);
    short_window.erase("windowSeconds");
    short_window["sampledAtUtc"] = utc_now();
    short_window["oneMinute"] = window(60);
    short_window["fiveMinute"] = window(300);
    const auto process = process_metrics();
    return Json{{"eventLoop", short_window},
                {"process",
                 {{"pid", process_id()},
                  {"uptimeSeconds", std::chrono::duration<double>(now - started_).count()},
                  {"memory",
                   {{"rss", process.rss},
                    {"private", process.private_bytes},
                    {"allocator", allocator_snapshot()},
                    {"heapTotal", 0},
                    {"heapUsed", 0},
                    {"external", 0},
                    {"arrayBuffers", 0}}},
                  {"cpuTotalMs", process.cpu},
                  {"platform", process.platform}}}};
}
void PerformanceMonitor::persist() {
    std::lock_guard persistence(persist_mutex_);
    const auto value = capture();
    {
        std::lock_guard lock(mutex_);
        cache_ = value;
        cached_at_ = Clock::now();
    }
    std::optional<std::string> failure;
    try {
        write_json_atomic(state_path_, value);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    const auto build = build_();
    Json compact{{"sampledAtUtc", pointer_or_null(value, "/eventLoop/sampledAtUtc")},
                 {"gitSha", build.value("gitSha", Json())},
                 {"deploymentGeneration", build.value("deploymentGeneration", Json())},
                 {"pid", process_id()}};
    for (const auto* key : {"p95Ms", "p99Ms", "maxMs", "timerDriftMaxMs"})
        compact[key] = value["eventLoop"][key];
    compact["rss"] = value["process"]["memory"]["rss"];
    compact["private"] = value["process"]["memory"]["private"];
    compact["allocatorCurrent"] = pointer_or_null(value, "/process/memory/allocator/currentRequestedBytes");
    compact["cpuTotalMs"] = value["process"]["cpuTotalMs"];
    try {
        history_.append(compact);
    } catch (const std::exception& e) {
        if (failure)
            *failure += "; history=" + std::string(e.what());
        else
            failure = e.what();
    }
    if (failure)
        throw Error(*failure);
}
Json PerformanceMonitor::snapshot() const {
    std::lock_guard lock(mutex_);
    auto value = cache_;
    const auto age = std::chrono::duration_cast<Millis>(Clock::now() - cached_at_).count();
    value["cachedAgeMs"] = age;
    value["stale"] = age > 30000;
    return value;
}
} // namespace devbox
