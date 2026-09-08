#pragma once
#include "capture.hpp"
#include "contract.hpp"
#include "docker_files.hpp"
#include "lifecycle.hpp"
#include "monitoring.hpp"
#include "search.hpp"
#include "server.hpp"
namespace devbox {
class Engine final : public McpBackend, public std::enable_shared_from_this<Engine> {
    // Background callbacks stop before the services to which they refer are destroyed.
    BackgroundTasks background_;
    std::shared_ptr<const Config> config_;
    ToolContract contract_;
    std::set<std::string> implemented_;
    WorkPool commands_, files_{4, 128}, controls_{2, 128}, atomic_{2, 64};
    RuntimeExecutor runtime_;
    ExecutionScheduler scheduler_;
    JobManager jobs_;
    DockerFiles docker_files_;
    SearchService search_;
    LifecycleService lifecycle_;
    GithubAuthService github_;
    CaptureService capture_;
    UsageTelemetry usage_;
    PerformanceMonitor performance_;
    HttpServer* server_ = nullptr;
    OperationalMonitor monitoring_;
    std::atomic_bool stopped_{false};
    fs::path working_dir(const Json& args, bool host) const;
    void require_agent_host() const;
    asio::awaitable<ExecutionLease> acquire(AcquireRequest request, Cancel cancel);
    asio::awaitable<Json> execute(std::string name, Json args, Cancel cancel);
    asio::awaitable<Json> search(Json args, Cancel cancel);
    asio::awaitable<Json> capture(std::string name, Json args, Cancel cancel);
    asio::awaitable<Json> wait_file(Json args, Cancel cancel);
    asio::awaitable<Json> wait_job(Json args, Cancel cancel);
    Json files(std::string name, const Json& args, const Cancel& cancel);
    Json durable(std::string name, const Json& args);
    Json detached(std::string name, const Json& args);
    Json status(const Cancel& cancel);
    Json host_status() const;
    Json lifecycle(std::string name, const Cancel& cancel);

  public:
    explicit Engine(std::shared_ptr<const Config> config);
    ~Engine();
    void attach(HttpServer& server);
    void stop();
    Json server_info() const override;
    Json list_tools(std::string_view protocol) const override;
    bool has_tool(std::string_view name) const override {
        return implemented_.contains(std::string(name));
    }
    Json parity_report() const;
    asio::awaitable<Json> call_tool(std::string name, Json arguments, Cancel cancel) override;
    asio::awaitable<Json> metadata(const HttpRequest& request) override;
    bool ready() const override;
    std::string tool_started(const std::string& name, const Json& args, const Json& context) override;
    void tool_finished(const std::string& id, const Json& result) override;
    void tool_failed(const std::string& id, const std::string& error) override;
    void observe_http(const HttpRequest& request, int status, std::uint64_t bytes, Millis duration,
                      bool disconnected) override;
};
Json render_process_error(const std::exception& error, std::size_t max_chars, std::optional<Json> data = {});
Json render_file_output(std::string summary, const ProcessOutput& result, std::size_t max_chars);
} // namespace devbox
