#pragma once
#include "background.hpp"
#include "gateway.hpp"
#include <deque>
#include <map>
namespace devbox {
// A sink is owned by one writer; no logging path holds application-state locks.
class JsonLogSink {
    fs::path path_;
    std::uint64_t maximum_;
    std::size_t rotations_;

  public:
    JsonLogSink(fs::path path, std::uint64_t maximum, std::size_t rotations);
    void append(const Json& event);
    void append_batch(std::span<const Json> events);
};
class UsageLogger {
    JsonLogSink sink_;
    BackgroundTasks& background_;
    std::string name_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Json> queue_;
    std::thread thread_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> enqueued_{0}, dropped_{0}, failures_{0};
    void run();

  public:
    UsageLogger(fs::path path, std::uint64_t maximum, std::size_t rotations, BackgroundTasks& background,
                std::string name);
    ~UsageLogger();
    void enqueue(Json event);
    void stop();
    Json snapshot() const;
};
Json summarize_arguments(const Json& arguments);
class UsageTelemetry {
    struct Invocation {
        std::string id, tool, started_at;
        Clock::time_point start = Clock::now();
        Json arguments, context;
        Json event(std::string type) const;
    };
    UsageLogger tools_, http_;
    mutable std::mutex mutex_;
    std::map<std::string, Invocation> active_;
    std::optional<Invocation> remove(const std::string& id);

  public:
    UsageTelemetry(const Config& config, BackgroundTasks& background);
    std::string started(const std::string& tool, const Json& args, const Json& context);
    void finished(const std::string& id, const Json& response);
    void failed(const std::string& id, const std::string& error);
    void http(const HttpRequest& request, int status, Millis duration, bool disconnected);
    Json active_tools() const;
    Json snapshot() const;
    void stop();
};
Json allocator_snapshot();
class PerformanceMonitor {
    struct Sample {
        Clock::time_point at;
        double ms;
    };
    mutable std::mutex mutex_;
    std::deque<Sample> delays_, drifts_;
    std::mutex persist_mutex_;
    Json cache_;
    Clock::time_point started_ = Clock::now(), cached_at_ = Clock::now();
    BackgroundTasks& background_;
    fs::path state_path_;
    JsonLogSink history_;
    std::function<Json()> build_;
    std::atomic_bool attached_{false};
    asio::awaitable<void> sample(Cancel cancel);
    Json capture();

  public:
    PerformanceMonitor(const Config& config, BackgroundTasks& background, std::function<Json()> build);
    // The executor must belong to the serving HTTP runtime.
    void attach(asio::any_io_executor executor, Cancel cancel);
    void persist();
    Json snapshot() const;
};
} // namespace devbox
