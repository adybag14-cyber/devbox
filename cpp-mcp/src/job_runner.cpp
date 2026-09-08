#include "devbox/scoped_thread.hpp"
#include "devbox/jobs.hpp"
#include <algorithm>
#include <csignal>
#include <thread>
namespace devbox {
namespace {
volatile std::sig_atomic_t runner_signal = 0;
void runner_signal_handler(int) {
    runner_signal = 1;
}
class RunnerMonitor {
    JobStore store_;
    std::string id_, runtime_, status_ = "queued";
    std::uint32_t child_ = 0;
    std::optional<std::uint64_t> child_instance_;
    std::mutex mutex_, write_mutex_;
    Cancel stop_ = std::make_shared<Cancellation>();
    Cancel cancel_ = std::make_shared<Cancellation>();
    ScopedThread thread_;
    void write() {
        std::lock_guard writer(write_mutex_);
        Json value;
        {
            std::lock_guard lock(mutex_);
            value = Json{{"pid", process_id()},
                         {"status", status_},
                         {"childPid", child_ ? Json(child_) : Json(nullptr)},
                         {"childProcessInstance", child_instance_ ? Json(*child_instance_) : Json(nullptr)},
                         {"runtimeMode", runtime_},
                         {"updatedAtUtc", utc_now()}};
        }
        store_.write_heartbeat(id_, value);
    }

  public:
    RunnerMonitor(JobStore store, const Json& request, std::uint64_t interval)
        : store_(std::move(store)), id_(request["id"].get<std::string>()),
          runtime_(request["runtimeMode"].get<std::string>()) {
        write();
        thread_ = ScopedThread([this, interval] {
            auto next_heartbeat = Clock::now() + Millis(std::max<std::uint64_t>(1000, interval));
            while (!stop_->wait_for(Millis(50))) {
                try {
                    if (runner_signal || store_.cancellation_requested(id_))
                        cancel_->cancel();
                    if (Clock::now() >= next_heartbeat) {
                        write();
                        next_heartbeat = Clock::now() + Millis(std::max<std::uint64_t>(1000, interval));
                    }
                } catch (...) {
                    // A heartbeat failure is visible as staleness; do not claim cancellation succeeded.
                }
            }
        });
    }
    ~RunnerMonitor() {
        stop_->cancel();
        if (thread_.joinable())
            thread_.join();
    }
    Cancel cancellation() const {
        return cancel_;
    }
    void set_status(std::string value) {
        {
            std::lock_guard lock(mutex_);
            status_ = std::move(value);
        }
        write();
    }
    void set_child(std::uint32_t pid) {
        const auto instance = process_instance(pid);
        {
            std::lock_guard lock(mutex_);
            child_ = pid;
            child_instance_ = instance;
        }
        write();
    }
    std::optional<std::uint32_t> child() {
        std::lock_guard lock(mutex_);
        return child_ ? std::optional(child_) : std::nullopt;
    }
    void finish(std::string status) {
        set_status(std::move(status));
        stop_->cancel();
        if (thread_.joinable())
            thread_.join();
    }
};
Json queued_status(const Json& request, const std::string& queued) {
    const auto instance = process_instance(process_id());
    return Json{{"id", request["id"]},
                {"status", "queued"},
                {"mode", request["mode"]},
                {"createdAtUtc", request["createdAtUtc"]},
                {"queuedAtUtc", queued},
                {"startedAtUtc", nullptr},
                {"completedAtUtc", nullptr},
                {"runnerPid", process_id()},
                {"runnerProcessInstance", instance ? Json(std::to_string(*instance)) : Json(nullptr)},
                {"exitCode", nullptr},
                {"readOnly", json_bool(request, "readOnly")},
                {"resourceClass", request["resourceClass"]},
                {"runtimeMode", request["runtimeMode"]}};
}
void add_lease(Json& value, const std::optional<ExecutionLease>& lease,
               std::optional<std::size_t> weight = {}) {
    value["queueWaitMs"] = lease ? Json(lease->queue_wait_ms) : Json(nullptr);
    value["executionSlot"] = lease && !lease->slots.empty() ? Json(lease->slots.front()) : Json(nullptr);
    value["executionSlots"] = lease ? Json(lease->slots) : Json(nullptr);
    value["executionPool"] = lease ? Json(lease->pool) : Json(nullptr);
    value["executionWeight"] = lease ? Json(lease->weight) : weight ? Json(*weight) : Json(nullptr);
}
void persist_terminal(JobStore& store, const std::string& id, const Json& final) {
    std::optional<Json> current;
    try {
        current = store.read_status_raw(id);
    } catch (...) {
    }
    if (!current || json_string(*current, "status") != "cancelled")
        store.write_status(id, final);
}
} // namespace
int run_job_request(std::shared_ptr<const Config> config, const fs::path& request_path) {
    const auto request = read_json(request_path, 16 * 1024 * 1024);
    if (!request.is_object())
        throw Error("Job request must be a JSON object");
    const auto id = request.at("id").get<std::string>();
    const auto mode = request.at("mode").get<std::string>();
    if (mode != "shell" && mode != "program")
        throw Error("Job mode must be shell or program");
    const auto class_name = request.at("resourceClass").get<std::string>();
    if (class_name != "watch" && class_name != "light" && class_name != "heavy" && class_name != "io-heavy")
        throw Error("Unknown job resource class");
    request.at("runtimeMode").get<std::string>();
    request.at("createdAtUtc").get<std::string>();
    const auto timeout_ms = json_uint(request, "timeoutMs");
    if (timeout_ms > static_cast<std::uint64_t>(INT64_MAX))
        throw Error("Job timeout exceeds the supported range");
    JobStore store(config);
    const auto paths = store.paths(id);
    if (canonical_target(paths.request) != canonical_target(request_path))
        throw Error("C++ job request path does not match configured job root path.");
    // An exclusive runner lock prevents a second CLI invocation from running the same request.
    FileLock runner_lock(paths.dir / ".runner.lock", Millis(1));
    const auto initial = store.read_status_raw(id);
    if (terminal_status(json_string(initial, "status")) || store.cancellation_requested(id))
        return 0;
    if (const auto existing = json_uint(initial, "runnerPid");
        existing && existing <= UINT32_MAX && existing != process_id()) {
        const auto instance =
            initial.contains("runnerProcessInstance")
                ? owner_process_instance(Json{{"processInstance", initial["runnerProcessInstance"]}})
                : std::nullopt;
        if (instance && process_matches_instance(static_cast<std::uint32_t>(existing), instance))
            return 0;
    }
    const auto queued = utc_now();
    store.write_status(id, queued_status(request, queued));
    runner_signal = 0;
    const auto previous_term = std::signal(SIGTERM, runner_signal_handler);
    const auto previous_interrupt = std::signal(SIGINT, runner_signal_handler);
    ScopeExit restore_signals([&] {
        std::signal(SIGTERM, previous_term);
        std::signal(SIGINT, previous_interrupt);
    });
    RunnerMonitor monitor(store, request, config->job_heartbeat_ms);
    JobLogPump logs(paths.stdout_log, paths.stderr_log, config->job_log_max_bytes, config->job_log_rotations);
    auto scheduler_config = SchedulerConfig::from(*config);
    scheduler_config.queue_timeout = Millis(config->background_queue_timeout_ms);
    ExecutionScheduler scheduler(scheduler_config);
    const auto resource = resource_class(class_name);
    const auto weight = resource == ResourceClass::heavy ? std::max<std::size_t>(1, config->exec_heavy_weight)
                        : resource == ResourceClass::io_heavy
                            ? std::max<std::size_t>(1, config->exec_io_heavy_weight)
                            : 1;
    const auto cancellation = monitor.cancellation();
    std::optional<ExecutionLease> lease;
    try {
        lease = scheduler.acquire({ExecutionKind::background, resource, weight, "devbox_job:" + id, {}},
                                  cancellation);
    } catch (const std::exception& error) {
        auto final = queued_status(request, queued);
        const auto status =
            cancellation->cancelled() || store.cancellation_requested(id) ? "cancelled" : "failed";
        final["status"] = status;
        final["completedAtUtc"] = utc_now();
        add_lease(final, lease, weight);
        final["childPid"] = nullptr;
        final["error"] = error.what();
        final["logs"] = logs.finish();
        persist_terminal(store, id, final);
        monitor.finish(status);
        return 0;
    }
    if (cancellation->cancelled() || store.cancellation_requested(id)) {
        lease->release();
        auto final = queued_status(request, queued);
        final["status"] = "cancelled";
        final["completedAtUtc"] = utc_now();
        add_lease(final, lease);
        final["childPid"] = nullptr;
        final["logs"] = logs.finish();
        persist_terminal(store, id, final);
        monitor.finish("cancelled");
        return 0;
    }
    const auto started = utc_now();
    auto final = queued_status(request, queued);
    final["status"] = "running";
    final["startedAtUtc"] = started;
    add_lease(final, lease);
    final["childPid"] = nullptr;
    store.write_status(id, final);
    monitor.set_status("running");
    auto working_dir = trim(json_string(request, "workingDir"));
    if (working_dir.empty())
        working_dir = path_text(config->runtime_mode == RuntimeMode::host ? config->host_default_workdir
                                                                          : config->devbox_workspace_path);
    auto user = trim(json_string(request, "user"));
    if (user.empty())
        user = config->devbox_default_user;
    const auto on_output = [&](OutputStream stream, std::string_view bytes) { logs.push(stream, bytes); };
    const auto on_pid = [&](std::uint32_t pid) { monitor.set_child(pid); };
    RuntimeExecutor runtime(config);
    std::string status = "succeeded";
    try {
        ProcessOutput output;
        if (mode == "program") {
            ProgramRequest options;
            options.program = json_string(request, "program");
            options.args = json_strings(request, "args");
            if (request.contains("input") && !request["input"].is_null())
                options.input = request["input"].get<std::string>();
            options.working_dir = path_from_utf8(working_dir);
            options.timeout = Millis(timeout_ms);
            options.user = user;
            options.max_capture_chars = 65536;
            options.on_output = on_output;
            options.on_pid = on_pid;
            output = runtime.run_program(std::move(options), cancellation);
        } else {
            ShellRequest options;
            options.command = json_string(request, "command");
            options.working_dir = path_from_utf8(working_dir);
            options.timeout = Millis(timeout_ms);
            options.user = user;
            options.max_capture_chars = 65536;
            options.on_output = on_output;
            options.on_pid = on_pid;
            output = runtime.run_shell(options, cancellation);
        }
        final["exitCode"] = output.exit_code;
    } catch (const ProcessError& error) {
        status = cancellation->cancelled() || store.cancellation_requested(id) || error.aborted ? "cancelled"
                 : error.timed_out                                                              ? "timed_out"
                                                                                                : "failed";
        final["exitCode"] = error.exit_code ? Json(*error.exit_code) : Json(nullptr);
        final["error"] = error.what();
    } catch (const std::exception& error) {
        status = cancellation->cancelled() || store.cancellation_requested(id) ? "cancelled" : "failed";
        final["error"] = error.what();
    }
    lease->release();
    final["status"] = status;
    final["completedAtUtc"] = utc_now();
    const auto child = monitor.child();
    final["childPid"] = child ? Json(*child) : Json(nullptr);
    final["logs"] = logs.finish();
    persist_terminal(store, id, final);
    monitor.finish(status);
    return 0;
}
} // namespace devbox
