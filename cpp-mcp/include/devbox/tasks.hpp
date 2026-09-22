#pragma once
#include "run_service.hpp"
namespace devbox {
inline constexpr std::string_view tasks_extension = "io.modelcontextprotocol/tasks";
bool client_supports_tasks(const Json& params);
class TaskService {
    std::shared_ptr<const Config> config_;
    JobStore jobs_;
    Json detail(std::string_view principal, std::string_view task_id);

  public:
    explicit TaskService(std::shared_ptr<const Config> config) : config_(config), jobs_(std::move(config)) {}
    Json augment(std::string_view principal, std::string_view tool, const Json& arguments,
                 const Json& tool_result);
    Json call(std::string_view principal, std::string_view method, const Json& params);
};
} // namespace devbox
