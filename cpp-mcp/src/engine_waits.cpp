#include "devbox/engine.hpp"
#include "devbox/result.hpp"
#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#endif
namespace devbox {
namespace {
Json path_state(const fs::path& path) {
    Json result;
    std::uint64_t size = 0;
    double millis = 0;
    bool valid_time = false, file = false, directory = false;
#ifdef _WIN32
    NativeHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return Json{{"exists", false}};
        if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION)
            return Json{{"exists", nullptr}, {"transientError", "EPERM"}};
        throw Error(windows_error(code));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info))
        throw Error(windows_error());
    directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    file = !directory;
    size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto ticks =
        (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    if (ticks >= 116444736000000000ULL) {
        millis = static_cast<double>(ticks - 116444736000000000ULL) / 10000.0;
        valid_time = true;
    }
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return Json{{"exists", false}};
        if (errno == EACCES || errno == EPERM)
            return Json{{"exists", nullptr}, {"transientError", "EPERM"}};
        throw Error(std::error_code(errno, std::generic_category()).message());
    }
    directory = S_ISDIR(info.st_mode);
    file = S_ISREG(info.st_mode);
    size = static_cast<std::uint64_t>(info.st_size);
#ifdef __APPLE__
    const auto stamp = info.st_mtimespec;
#else
    const auto stamp = info.st_mtim;
#endif
    if (stamp.tv_sec >= 0) {
        millis = static_cast<double>(stamp.tv_sec) * 1000.0 + static_cast<double>(stamp.tv_nsec) / 1000000.0;
        valid_time = true;
    }
#endif
    result = Json{{"exists", true}, {"isFile", file}, {"isDirectory", directory}, {"size", size}};
    if (valid_time) {
        result["mtimeMs"] = millis;
        result["mtimeUtc"] = utc_from_millis(static_cast<std::int64_t>(millis));
    }
    return result;
}
} // namespace
asio::awaitable<Json> Engine::wait_file(Json args, Cancel cancel) {
    if (config_->runtime_mode == RuntimeMode::docker)
        co_return result_error(
            "devbox_wait_for_file is a no-subprocess host-mode primitive; use devbox_wait in Docker mode.");
    const auto requested = json_string(args, "path");
    const auto path = path_from_utf8(requested);
    const auto start = Clock::now(),
               deadline =
                   start + Millis(static_cast<Millis::rep>(json_number(args, "timeout_seconds", 60) * 1000));
    std::optional<Clock::time_point> stable;
    try {
        for (;;) {
            auto pending = files_.run([path] { return path_state(path); }, cancel);
            auto state = co_await std::move(pending);
            const bool condition =
                json_bool(args, "should_exist", true)
                    ? json_bool(state, "exists") && json_uint(state, "size") >= json_uint(args, "min_bytes")
                    : state.contains("exists") && state["exists"] == false;
            const auto now = Clock::now();
            if (condition) {
                if (!stable)
                    stable = now;
            } else
                stable.reset();
            const auto stable_ms = json_uint(args, "stable_ms");
            const bool met = stable && std::chrono::duration_cast<Millis>(now - *stable).count() >=
                                           static_cast<Millis::rep>(stable_ms);
            if (met || now >= deadline) {
                state["conditionMet"] = met;
                if (met && stable_ms)
                    state["stableMs"] = stable_ms;
                state["waitedMs"] = std::chrono::duration_cast<Millis>(now - start).count();
                state["timedOut"] = !met;
                const auto summary = met ? "File condition satisfied for " + requested + "."
                                         : "Timed out waiting for file condition at " + requested + ".";
                co_return result_success(summary, state);
            }
            co_await async_delay(std::min(Millis(json_uint(args, "poll_ms", 250)),
                                          std::chrono::duration_cast<Millis>(deadline - now)),
                                 cancel);
        }
    } catch (const Cancelled&) {
        co_return result_error("Wait for " + requested + " was cancelled.");
    } catch (const std::exception& e) {
        co_return result_error("Failed while waiting for " + requested + ": " + e.what());
    }
}
asio::awaitable<Json> Engine::wait_job(Json args, Cancel cancel) {
    const auto id = json_string(args, "job_id");
    const auto wait = Millis(static_cast<Millis::rep>(json_number(args, "wait_seconds") * 1000));
    const auto deadline = Clock::now() + wait;
    try {
        const auto store = jobs_.store();
        Json state;
        if (wait.count() == 0) {
            auto pending = files_.run([store, id] { return store.get_status(id); }, cancel);
            state = co_await std::move(pending);
        } else {
            auto initial_read =
                files_.run_until([store, id] { return store.read_status_raw(id); }, deadline, cancel);
            auto raw = co_await std::move(initial_read);
            if (!raw)
                throw Error("Job status wait exceeded its " + std::to_string(wait.count() / 1000) +
                            "s deadline before an initial state could be read.");
            state = std::move(*raw);
            auto reconcile = files_.run_until([store, id] { return store.get_status(id); }, deadline, cancel);
            if (auto reconciled = co_await std::move(reconcile))
                state = std::move(*reconciled);
            else {
                state["waitTimedOut"] = true;
                state["waitedMs"] = wait.count();
            }
        }
        const auto initial = json_string(state, "status");
        if (wait.count() > 0 && !terminal_status(initial)) {
            for (;;) {
                const auto now = Clock::now();
                if (now >= deadline) {
                    state["waitTimedOut"] = true;
                    state["waitedMs"] = wait.count();
                    break;
                }
                const auto elapsed = now - (deadline - wait);
                const auto interval = elapsed < Millis(1000)   ? Millis(50)
                                      : elapsed < Millis(5000) ? Millis(100)
                                                               : Millis(500);
                co_await async_delay(std::min(interval, std::chrono::duration_cast<Millis>(deadline - now)),
                                     cancel);
                auto next = files_.run_until([store, id] { return store.get_status(id); }, deadline, cancel);
                if (auto current = co_await std::move(next))
                    state = std::move(*current);
                else {
                    state["waitTimedOut"] = true;
                    state["waitedMs"] = wait.count();
                    break;
                }
                if (terminal_status(json_string(state, "status")) ||
                    (!json_bool(args, "terminal_only", true) && json_string(state, "status") != initial))
                    break;
            }
        }
        co_return result_success(
            "Background job " + id + " is " + json_string(state, "status", "unknown") + ".", state);
    } catch (const Cancelled&) {
        co_return result_error("Failed to fetch detached job " + id +
                               ": Job status wait cancelled by the MCP client.");
    } catch (const std::exception& e) {
        co_return result_error("Failed to fetch detached job " + id + ": " + e.what());
    }
}
} // namespace devbox
