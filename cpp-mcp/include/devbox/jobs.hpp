#pragma once
#include "runtime.hpp"
#include "scheduler.hpp"
namespace devbox {
struct JobPaths {
    std::string id;
    fs::path dir, request, status, stdout_log, stderr_log, cancel, heartbeat;
};
bool terminal_status(std::string_view status);
std::string validate_job_id(std::string_view value);
std::vector<std::string> job_ids(const fs::path& root, std::size_t maximum = 10000);
class JobLogPump {
    struct State;
    std::unique_ptr<State> state_;

  public:
    JobLogPump(const fs::path& stdout_path, const fs::path& stderr_path, std::uint64_t max_bytes,
               std::size_t rotations);
    ~JobLogPump();
    JobLogPump(const JobLogPump&) = delete;
    JobLogPump& operator=(const JobLogPump&) = delete;
    void push(OutputStream stream, std::string_view bytes);
    Json finish();
};
fs::path rotated_log_path(const fs::path& path, std::size_t index);
Json log_metadata(const fs::path& path, std::size_t rotations);
std::string read_log_tail(const fs::path& path, std::size_t max_chars, std::size_t rotations);
class JobStore {
    struct Maintenance;
    std::shared_ptr<const Config> config_;
    std::shared_ptr<Maintenance> maintenance_;
    Json reconcile(const JobPaths& paths, Json value) const;
    Json reconcile_cancelled(const JobPaths& paths, Json value) const;
    Json interrupt_orphan(const JobPaths& paths, Json value, const std::optional<Json>& heartbeat,
                          std::optional<Millis> age) const;

  public:
    explicit JobStore(std::shared_ptr<const Config> config);
    const Config& config() const {
        return *config_;
    }
    JobPaths paths(std::string_view id) const;
    JobPaths create_job(std::string_view id, const Json& request, const Json& status);
    Json read_request(std::string_view id) const;
    Json read_status_raw(std::string_view id) const;
    void write_status(std::string_view id, const Json& status) const;
    void write_heartbeat(std::string_view id, const Json& heartbeat) const;
    bool cancellation_requested(std::string_view id) const;
    Json get_status(std::string_view id) const;
    Json wait_status(std::string_view id, Millis wait, bool terminal_only, Millis poll,
                     const Cancel& cancel = {}) const;
    Json logs(std::string_view id, std::size_t max_chars) const;
    Json cancel(std::string_view id) const;
    Json list(const std::optional<std::string>& task, const std::vector<std::string>& statuses,
              const std::optional<std::string>& cursor, std::size_t limit) const;
    void admit(const std::optional<std::string>& task = {}) const;
    Json reconcile_maintenance(std::size_t maximum = 32);
    Json enforce_store_quota();
    Json quota_snapshot() const;
};
struct Submission {
    std::string task_id, operation_id, label;
    Json json() const {
        return Json{{"taskId", task_id}, {"operationId", operation_id}, {"label", label}};
    }
};
ResourceClass infer_shell_resource(std::string_view command, std::string_view requested = "auto");
ResourceClass infer_program_resource(std::string_view program, const std::vector<std::string>& args,
                                     std::string_view requested = "auto");
class JobManager {
    std::shared_ptr<const Config> config_;
    JobStore store_;
    Json persist_and_spawn(Json request, const std::optional<Submission>& agent);
    Json submit(Json request, const Submission& agent);
    Json shell_request(const ShellRequest& options, std::string_view resource, bool read_only) const;
    Json program_request(const ProgramRequest& options, std::string_view resource) const;

  public:
    explicit JobManager(std::shared_ptr<const Config> config) : config_(config), store_(std::move(config)) {}
    JobStore& store() {
        return store_;
    }
    const JobStore& store() const {
        return store_;
    }
    Json start_shell(const ShellRequest& options, std::string_view resource = "auto", bool read_only = false);
    Json start_program(const ProgramRequest& options, std::string_view resource = "auto");
    Json submit_shell(const ShellRequest& options, const Submission& agent,
                      std::string_view resource = "auto");
    Json submit_program(const ProgramRequest& options, const Submission& agent,
                        std::string_view resource = "auto");
};
int run_job_request(std::shared_ptr<const Config> config, const fs::path& path);
} // namespace devbox
