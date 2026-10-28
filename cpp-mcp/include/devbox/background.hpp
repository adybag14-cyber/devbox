#pragma once
#include "async.hpp"
#include <boost/asio/io_context.hpp>
#include <map>
namespace devbox {
class BackgroundTasks {
    struct State {
        std::string kind;
        bool running = false;
        std::uint64_t starts = 0, restarts = 0, consecutive_failures = 0;
        std::optional<std::uint64_t> last_tick, last_attempt, last_success, last_failure;
        std::optional<std::string> last_error;
    };
    asio::io_context io_{1};
    WorkPool workers_{3, 32};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread thread_;
    Cancel cancel_ = std::make_shared<Cancellation>();
    mutable std::mutex mutex_;
    std::map<std::string, State> states_;
    bool started_ = false;
    State& state_locked(const std::string& name);
    asio::awaitable<void> run_periodic(std::string name, Millis initial, Millis interval,
                                       std::function<void(const Cancel&)> action, bool once);
    asio::awaitable<void> run_adaptive(std::string name, Millis initial,
                                       std::function<Millis(const Cancel&)> action);

  public:
    BackgroundTasks() = default;
    ~BackgroundTasks();
    void start();
    void stop();
    void periodic(std::string name, Millis initial, Millis interval,
                  std::function<void(const Cancel&)> action);
    void once(std::string name, Millis delay, std::function<void(const Cancel&)> action);
    void adaptive(std::string name, Millis initial, std::function<Millis(const Cancel&)> action);
    void mark_started(const std::string& name, std::string kind = "event-driven");
    void mark_stopped(const std::string& name, std::optional<std::string> error = {});
    void attempt(const std::string& name);
    void success(const std::string& name);
    void failure(const std::string& name, const std::string& error);
    Json snapshot() const;
};
} // namespace devbox
