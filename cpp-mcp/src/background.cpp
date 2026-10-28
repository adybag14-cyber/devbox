#include "devbox/background.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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
BackgroundTasks::State& BackgroundTasks::state_locked(const std::string& name) {
    return states_.try_emplace(name).first->second;
}
void BackgroundTasks::mark_started(const std::string& name, std::string kind) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state.kind = std::move(kind);
    if (state.starts++)
        ++state.restarts;
    state.running = true;
    state.last_attempt = unix_millis();
}
void BackgroundTasks::mark_stopped(const std::string& name, std::optional<std::string> error) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state.running = false;
    if (error) {
        state.last_failure = unix_millis();
        ++state.consecutive_failures;
        state.last_error = std::move(error);
    }
}
void BackgroundTasks::attempt(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state.running = true;
    state.last_attempt = unix_millis();
}
void BackgroundTasks::success(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    const auto now = unix_millis();
    state.running = true;
    state.last_tick = now;
    state.last_success = now;
    state.consecutive_failures = 0;
    state.last_error.reset();
}
void BackgroundTasks::failure(const std::string& name, const std::string& error) {
    std::lock_guard lock(mutex_);
    auto& state = state_locked(name);
    state.running = true;
    state.last_failure = unix_millis();
    ++state.consecutive_failures;
    state.last_error = error;
}
Json BackgroundTasks::snapshot() const {
    std::lock_guard lock(mutex_);
    Json::object_t workers;
    workers.reserve(states_.size());
    const auto now = unix_millis();
    for (const auto& [name, state] : states_) {
        Json::object_t fields;
        // These fixed fields and the names in states_ are already unique.
        // Append directly to ordered_json's vector so constructing a snapshot
        // does not repeatedly scan earlier fields or worker names for duplicates.
        fields.reserve(14);
        const auto timestamp = [](const std::optional<std::uint64_t>& at) -> Json {
            return at ? Json(*at) : Json(nullptr);
        };
        const auto age = [&](const std::optional<std::uint64_t>& at) -> Json {
            return at ? Json(now - std::min(now, *at)) : Json(nullptr);
        };
        fields.emplace_back("kind", state.kind);
        fields.emplace_back("running", state.running);
        fields.emplace_back("starts", state.starts);
        fields.emplace_back("restarts", state.restarts);
        fields.emplace_back("lastTickUnixMs", timestamp(state.last_tick));
        fields.emplace_back("lastAttemptUnixMs", timestamp(state.last_attempt));
        fields.emplace_back("lastSuccessUnixMs", timestamp(state.last_success));
        fields.emplace_back("lastFailureUnixMs", timestamp(state.last_failure));
        fields.emplace_back("consecutiveFailures", state.consecutive_failures);
        fields.emplace_back("lastError", state.last_error ? Json(*state.last_error) : Json(nullptr));
        fields.emplace_back("lastTickAgeMs", age(state.last_tick));
        fields.emplace_back("lastSuccessAgeMs", age(state.last_success));
        fields.emplace_back("lastFailureAgeMs", age(state.last_failure));
        fields.emplace_back("idleForMs", state.kind == "event-driven"
                                             ? Json(now - std::min(now, state.last_attempt.value_or(now)))
                                             : Json(nullptr));
        workers.emplace_back(name, Json(std::move(fields)));
    }
    return Json(std::move(workers));
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
