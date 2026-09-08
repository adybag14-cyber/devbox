#include "devbox/background.hpp"
namespace devbox {
asio::awaitable<void> BackgroundTasks::run_adaptive(std::string name, Millis initial,
                                                    std::function<Millis(const Cancel&)> action) {
    mark_started(name, "periodic");
    auto delay = initial;
    try {
        while (!cancel_->cancelled()) {
            co_await async_delay(delay, cancel_);
            attempt(name);
            try {
                auto pending = workers_.run([action, cancel = cancel_] { return action(cancel); }, cancel_);
                delay = co_await std::move(pending);
                delay = std::max(Millis(1), delay);
                success(name);
            } catch (const Cancelled&) {
                break;
            } catch (const std::exception& e) {
                failure(name, e.what());
                delay = Millis(10000);
            }
        }
        mark_stopped(name);
    } catch (const Cancelled&) {
        mark_stopped(name);
    } catch (const std::exception& e) {
        mark_stopped(name, e.what());
    }
}
void BackgroundTasks::adaptive(std::string name, Millis initial,
                               std::function<Millis(const Cancel&)> action) {
    start();
    asio::co_spawn(io_, run_adaptive(std::move(name), initial, std::move(action)), [](std::exception_ptr) {});
}
BackgroundTasks::~BackgroundTasks() {
    stop();
}
void BackgroundTasks::start() {
    std::lock_guard lock(mutex_);
    if (started_)
        return;
    started_ = true;
    work_.emplace(asio::make_work_guard(io_));
    thread_ = std::thread([this] { io_.run(); });
}
void BackgroundTasks::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!started_)
            return;
        started_ = false;
    }
    cancel_->cancel();
    work_.reset();
    if (thread_.joinable())
        thread_.join();
}
Json& BackgroundTasks::state_locked(const std::string& name) {
    auto [entry, inserted] = states_.try_emplace(name);
    if (inserted)
        entry->second = Json{{"kind", ""},
                             {"running", false},
                             {"starts", 0},
                             {"restarts", 0},
                             {"lastTickUnixMs", nullptr},
                             {"lastAttemptUnixMs", nullptr},
                             {"lastSuccessUnixMs", nullptr},
                             {"lastFailureUnixMs", nullptr},
                             {"consecutiveFailures", 0},
                             {"lastError", nullptr}};
    return entry->second;
}
void BackgroundTasks::mark_started(const std::string& name, std::string kind) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    const auto count = json_uint(state, "starts");
    state["kind"] = std::move(kind);
    state["starts"] = count + 1;
    if (count)
        state["restarts"] = json_uint(state, "restarts") + 1;
    state["running"] = true;
    state["lastAttemptUnixMs"] = unix_millis();
}
void BackgroundTasks::mark_stopped(const std::string& name, std::optional<std::string> error) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state["running"] = false;
    if (error) {
        state["lastFailureUnixMs"] = unix_millis();
        state["consecutiveFailures"] = json_uint(state, "consecutiveFailures") + 1;
        state["lastError"] = *error;
    }
}
void BackgroundTasks::attempt(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state["running"] = true;
    state["lastAttemptUnixMs"] = unix_millis();
}
void BackgroundTasks::success(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    const auto now = unix_millis();
    state["running"] = true;
    state["lastTickUnixMs"] = now;
    state["lastSuccessUnixMs"] = now;
    state["consecutiveFailures"] = 0;
    state["lastError"] = nullptr;
}
void BackgroundTasks::failure(const std::string& name, const std::string& error) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state["running"] = true;
    state["lastFailureUnixMs"] = unix_millis();
    state["consecutiveFailures"] = json_uint(state, "consecutiveFailures") + 1;
    state["lastError"] = error;
}
Json BackgroundTasks::snapshot() const {
    std::lock_guard lock(mutex_);
    Json result = Json::object();
    const auto now = unix_millis();
    for (const auto& [name, state] : states_) {
        auto value = state;
        const auto age = [&](const char* key) -> Json {
            return state[key].is_number() ? Json(now - std::min(now, json_uint(state, key))) : Json(nullptr);
        };
        value["lastTickAgeMs"] = age("lastTickUnixMs");
        value["lastSuccessAgeMs"] = age("lastSuccessUnixMs");
        value["lastFailureAgeMs"] = age("lastFailureUnixMs");
        value["idleForMs"] = json_string(state, "kind") == "event-driven"
                                 ? Json(now - std::min(now, json_uint(state, "lastAttemptUnixMs", now)))
                                 : Json(nullptr);
        result[name] = std::move(value);
    }
    return result;
}
asio::awaitable<void> BackgroundTasks::run_periodic(std::string name, Millis initial, Millis interval,
                                                    std::function<void(const Cancel&)> action, bool once) {
    mark_started(name, once ? "one-shot" : "periodic");
    try {
        co_await async_delay(initial, cancel_);
        while (!cancel_->cancelled()) {
            attempt(name);
            try {
                auto pending = workers_.run([action, cancel = cancel_] { action(cancel); }, cancel_);
                co_await std::move(pending);
                success(name);
            } catch (const Cancelled&) {
                break;
            } catch (const std::exception& e) {
                failure(name, e.what());
            }
            if (once)
                break;
            co_await async_delay(interval, cancel_);
        }
        mark_stopped(name);
    } catch (const Cancelled&) {
        mark_stopped(name);
    } catch (const std::exception& e) {
        mark_stopped(name, e.what());
    }
}
void BackgroundTasks::periodic(std::string name, Millis initial, Millis interval,
                               std::function<void(const Cancel&)> action) {
    start();
    asio::co_spawn(
        io_, run_periodic(std::move(name), initial, std::max(Millis(1), interval), std::move(action), false),
        asio::detached);
}
void BackgroundTasks::once(std::string name, Millis delay, std::function<void(const Cancel&)> action) {
    start();
    asio::co_spawn(io_, run_periodic(std::move(name), delay, Millis(0), std::move(action), true),
                   asio::detached);
}
} // namespace devbox
