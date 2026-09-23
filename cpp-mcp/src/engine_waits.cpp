#include "devbox/engine.hpp"
#include "devbox/filesystem_worker.hpp"
#include "devbox/result.hpp"
#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#endif
namespace devbox {
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
    Json state = Json::object();
    try {
        for (;;) {
            if (Clock::now() < deadline || state.empty()) {
                try {
                    auto pending = files_.run(
                        [path, cancel, deadline] {
                            return isolated_filesystem("path_state", Json{{"path", path_text(path)}},
                                                       std::max(Millis(1), std::chrono::duration_cast<Millis>(
                                                                               deadline - Clock::now())),
                                                       cancel);
                        },
                        cancel);
                    state = co_await std::move(pending);
                } catch (const FilesystemDeadline&) {
                    state = Json{{"exists", nullptr}, {"observationUnavailable", true}};
                }
            }
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
