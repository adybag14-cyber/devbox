#pragma once
#include "jobs.hpp"
#include "telemetry.hpp"
namespace devbox {
std::optional<std::uint64_t> snapshot_age(const Json& value, std::string_view key = "sampledAtUtc");
Json read_status_snapshot(const fs::path& path, std::uint64_t stale_ms = 150000);
Json guardian_snapshot(const Config& config);
Json degraded_background(const Json& snapshot);
std::string disk_pressure(std::uint64_t free, std::uint64_t total, bool valid);
bool cleanup_operation(std::string_view operation);
class OperationalMonitor {
    std::shared_ptr<const Config> config_;
    BackgroundTasks& background_;
    ExecutionScheduler& scheduler_;
    JobStore& jobs_;
    RuntimeExecutor& runtime_;
    PerformanceMonitor& performance_;
    UsageTelemetry& usage_;
    std::function<std::size_t()> active_requests_;
    mutable std::mutex mutex_;
    Json execution_ = nullptr,
         store_ =
             Json{{"ok", false}, {"sampledAtUtc", nullptr}, {"error", "Execution store probe has not run."}};
    std::deque<std::pair<Clock::time_point, std::uint64_t>> disk_samples_;
    Clock::time_point last_incident_{};
    JsonLogSink incidents_;
    void scheduler_iteration();
    void job_iteration(bool quota);
    void incident_iteration();

  public:
    OperationalMonitor(std::shared_ptr<const Config> config, BackgroundTasks& background,
                       ExecutionScheduler& scheduler, JobStore& jobs, RuntimeExecutor& runtime,
                       PerformanceMonitor& performance, UsageTelemetry& usage,
                       std::function<std::size_t()> active_requests);
    void start();
    void probe_store();
    Json execution_cache() const;
    std::optional<Json> execution() const;
    Json store_health() const;
    bool ready(std::size_t tools) const;
    std::optional<Json> reject_disk_work(ResourceClass resource, bool read_only,
                                         std::string_view operation) const;
};
} // namespace devbox
