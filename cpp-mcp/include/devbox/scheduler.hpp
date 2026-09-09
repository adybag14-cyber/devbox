#pragma once
#include "config.hpp"
#include "storage.hpp"
namespace devbox {
enum class ResourceClass { watch, light, heavy, io_heavy };
enum class ExecutionKind { interactive, background };
ResourceClass resource_class(std::string_view value);
std::string resource_name(ResourceClass value);
std::string execution_name(ExecutionKind value);
struct SchedulerConfig {
    fs::path root;
    std::size_t max_concurrent = 6, reserved_interactive = 1, watch_max_concurrent = 4;
    Millis queue_timeout{15000};
    std::size_t heavy_capacity = 4, heavy_weight = 2, io_heavy_capacity = 2, io_heavy_weight = 2;
    Millis background_priority_age{30000};
    static SchedulerConfig from(const Config& config);
    SchedulerConfig normalized() const;
};
struct AcquireRequest {
    ExecutionKind kind = ExecutionKind::interactive;
    ResourceClass resource_class = ResourceClass::light;
    std::size_t weight = 1;
    std::string label;
    std::optional<Millis> queue_timeout;
};
struct QueueTimeout : Error {
    Json details;
    QueueTimeout(std::string message, Json value) : Error(std::move(message)), details(std::move(value)) {}
};
struct QueueCancelled : Error {
    QueueCancelled() : Error("Execution queue wait cancelled by the MCP client.") {}
};
struct SchedulerMetrics;
class ExecutionLease {
    struct State;
    std::unique_ptr<State> state_;
    friend class ExecutionWaiter;

  public:
    ExecutionLease();
    ~ExecutionLease();
    ExecutionLease(ExecutionLease&&) noexcept;
    ExecutionLease& operator=(ExecutionLease&&) noexcept;
    ExecutionLease(const ExecutionLease&) = delete;
    ExecutionLease& operator=(const ExecutionLease&) = delete;
    std::vector<std::size_t> slots;
    ExecutionKind kind = ExecutionKind::interactive;
    ResourceClass resource_class = ResourceClass::light;
    std::string pool;
    std::size_t weight = 1;
    std::uint64_t queue_wait_ms = 0;
    void release();
    Json json() const;
};
class ExecutionWaiter {
    struct State;
    std::unique_ptr<State> state_;
    friend class ExecutionScheduler;
    explicit ExecutionWaiter(std::unique_ptr<State> state);

  public:
    ~ExecutionWaiter();
    ExecutionWaiter(ExecutionWaiter&&) noexcept;
    ExecutionWaiter& operator=(ExecutionWaiter&&) noexcept;
    ExecutionWaiter(const ExecutionWaiter&) = delete;
    ExecutionWaiter& operator=(const ExecutionWaiter&) = delete;
    // One bounded filesystem pass. Waiting belongs to a caller's coroutine or runner.
    std::optional<ExecutionLease> poll(const Cancel& cancel = {});
    Millis poll_interval() const;
};
class ExecutionScheduler {
    SchedulerConfig config_;
    std::shared_ptr<SchedulerMetrics> metrics_;

  public:
    explicit ExecutionScheduler(SchedulerConfig config);
    const SchedulerConfig& config() const {
        return config_;
    }
    ExecutionWaiter begin(AcquireRequest request) const;
    ExecutionLease acquire(AcquireRequest request, const Cancel& cancel = {}) const;
    Json snapshot() const;
};
std::optional<std::uint64_t> owner_process_instance(const Json& owner);
bool owner_process_alive(const Json& owner);
} // namespace devbox
