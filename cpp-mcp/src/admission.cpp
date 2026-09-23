#include "devbox/admission.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
namespace devbox {
namespace {
Json read_control(const fs::path& root) {
    if (!fs::exists(root))
        return Json{{"mode", "open"}, {"generation", "initial"}};
    ensure_private_state_directory(root);
    const auto value = read_json(root / "control.json", 4096);
    if ((json_string(value, "mode") != "open" && json_string(value, "mode") != "draining") ||
        json_string(value, "generation").size() != 36)
        throw Error("ADMISSION_CONTROL_INVALID");
    return Json{{"mode", value.at("mode")}, {"generation", value.at("generation")}};
}
} // namespace
Json operator_admission(const Config& config, std::string_view action) {
    const auto root = config.project_root / "run" / ".admission";
    if (action == "status")
        return read_control(root);
    if (action != "drain" && action != "resume")
        throw Error("Admission action must be drain, resume or status");
    ensure_directory(root.parent_path());
    ensure_private_state_directory(root);
    FileLock lock(root / ".operator.lock", Millis(2000), {}, true);
    const auto value = Json{{"mode", action == "drain" ? "draining" : "open"},
                            {"generation", uuid()},
                            {"requested_at", utc_now()}};
    write_json_atomic(root / "control.json", value);
    return Json{
        {"requested", value},
        {"acknowledged_by_frontend", false},
        {"next", "Wait for matching metadata.admission generation, then reconcile all existing work"}};
}
AdmissionControl::AdmissionControl(const Config& config) : root_(config.project_root / "run" / ".admission") {
    refresh();
}
void AdmissionControl::refresh() {
    Json next;
    try {
        next = read_control(root_);
    } catch (...) {
        next =
            Json{{"mode", "draining"}, {"generation", "invalid"}, {"error", "ADMISSION_CONTROL_UNREADABLE"}};
    }
    std::lock_guard lock(mutex_);
    state_ = std::move(next);
}
Json AdmissionControl::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}
bool AdmissionControl::permits(std::string_view tool, const Json& arguments) const {
    {
        std::lock_guard lock(mutex_);
        if (json_string(state_, "mode") == "open")
            return true;
    }
    // A drain keeps observation and cancellation available. It never cancels
    // acknowledged jobs/runs and does not imply that external effects stopped.
    for (const auto* allowed :
         {"devbox_status", "devbox_capabilities", "devbox_job_status", "devbox_job_logs", "devbox_job_list",
          "devbox_job_cancel", "devbox_task_get", "devbox_task_list", "host_status"})
        if (tool == allowed)
            return true;
    if (tool == "devbox_agent_run") {
        const auto action = json_string(arguments, "action");
        return action == "get" || action == "events" || action == "artifact" || action == "profiles" ||
               action == "pause" || action == "cancel";
    }
    return false;
}
} // namespace devbox
