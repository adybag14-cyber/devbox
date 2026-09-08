#pragma once
#include "background.hpp"
#include "runtime.hpp"
namespace devbox {
enum class LifecycleAction { start, stop, restart, recreate };
std::string lifecycle_name(LifecycleAction action);
Json host_runtime_info(const Config& config);
Json parse_docker_info(std::string_view text, std::string_view fallback_name);
std::vector<std::string> create_container_args(const Config& config);
class LifecycleService {
    std::shared_ptr<const Config> config_;
    BackgroundTasks& background_;
    std::timed_mutex gate_;
    using DockerRunner = std::function<ProcessOutput(const std::vector<std::string>&, const Cancel&)>;
    DockerRunner docker_;
    Json inspect(const Cancel& cancel) const;
    Json create(const Cancel& cancel) const;
    void remove(const std::string& name, const Cancel& cancel) const;
    void migrate_tmp(const std::string& retired, const Cancel& cancel) const;
    Json recreate(const Cancel& cancel);

  public:
    explicit LifecycleService(std::shared_ptr<const Config> config, BackgroundTasks& background,
                              DockerRunner runner = {});
    Json status(const Cancel& cancel = {}) const;
    Json control(LifecycleAction action, const Cancel& cancel = {});
    void set_guardian_desired_state(bool should_run, std::string_view source) const;
};
class GithubAuthService {
    std::shared_ptr<const Config> config_;
    RuntimeExecutor& runtime_;
    LifecycleService& lifecycle_;
    ProcessOutput run(bool host, const std::string& program, std::vector<std::string> args, Millis timeout,
                      const Cancel& cancel, std::optional<std::string> input = {}) const;
    std::string identity(bool host, const std::string& key, const Cancel& cancel) const;
    Json selected_status(const Cancel& cancel) const;
    std::pair<std::string, Json> host_context(const Cancel& cancel) const;
    void ensure_selected(const Cancel& cancel) const;

  public:
    GithubAuthService(std::shared_ptr<const Config> config, RuntimeExecutor& runtime,
                      LifecycleService& lifecycle)
        : config_(std::move(config)), runtime_(runtime), lifecycle_(lifecycle) {}
    Json status(const Cancel& cancel = {}) const;
    Json sync_from_host(const Cancel& cancel = {}) const;
};
} // namespace devbox
