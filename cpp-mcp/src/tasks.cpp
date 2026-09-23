#include "devbox/tasks.hpp"
#include "devbox/result.hpp"
namespace devbox {
bool client_supports_tasks(const Json& params) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object())
        return false;
    const auto& meta = params["_meta"];
    const auto found = meta.find("io.modelcontextprotocol/clientCapabilities");
    if (found == meta.end() || !found->is_object() || !found->contains("extensions") ||
        !(*found)["extensions"].is_object())
        return false;
    const auto entry = (*found)["extensions"].find(tasks_extension);
    return entry != (*found)["extensions"].end() && entry->is_object();
}
Json TaskService::augment(std::string_view principal, std::string_view tool, const Json& args,
                          const Json& reply) {
    if (!jobs_.indexed() || json_bool(reply, "isError"))
        return nullptr;
    const auto structured = reply.value("structuredContent", Json::object());
    const auto data = structured.value("data", Json::object());
    std::string kind, handle;
    if (tool == "devbox_agent_run" && json_string(args, "action") == "create") {
        kind = "run";
        handle = json_string(data, "run_id");
    } else if (tool == "devbox_job_submit" || tool == "devbox_web_research") {
        kind = "job";
        handle = json_string(data, "id");
    } else
        return nullptr;
    if (handle.empty())
        return nullptr;
    validate_key(principal);
    validate_key(handle);
    const auto task_id = "task-" + sha256(Json::array({kind, handle}).dump()).substr(0, 40);
    auto store = jobs_.index();
    if (const auto previous = store->get("mcp_task", task_id)) {
        if (previous->principal != principal)
            return nullptr; // Existing trusted job tools retain their legacy view.
    } else {
        const StateMutation value{
            StateRecord{"mcp_task", task_id, std::string(principal), kind, "working", 0,
                        Json{{"handle", handle}, {"tool", tool}, {"created_at", utc_now()}}},
            0};
        try {
            store->apply({&value, 1});
        } catch (...) {
            const auto raced = store->get("mcp_task", task_id);
            if (!raced || raced->principal != principal)
                throw;
        }
    }
    auto result = detail(principal, task_id);
    result.erase("result");
    result.erase("error");
    result.erase("inputRequests");
    result["resultType"] = "task";
    return result;
}
Json TaskService::detail(std::string_view principal, std::string_view id) {
    validate_key(principal);
    validate_key(id);
    if (!jobs_.indexed())
        throw Error("TASK_NOT_FOUND");
    auto store = jobs_.index();
    const auto binding = store->get("mcp_task", id);
    if (!binding || binding->principal != principal)
        throw Error("TASK_NOT_FOUND");
    if (binding->data.contains("terminal_result"))
        return binding->data["terminal_result"];
    const auto handle = json_string(binding->data, "handle");
    Json state;
    if (binding->group == "run")
        state = RunService(config_).call(principal, Json{{"action", "get"}, {"run_id", handle}});
    else
        state = jobs_.get_status(handle);
    const auto status = json_string(state, "status");
    const bool cancelled = status == "cancelled";
    const bool success = status == "completed" || status == "succeeded";
    const bool failed = status == "failed" || status == "timed_out" || status == "interrupted";
    const bool input = binding->group == "run" &&
                       (status == "awaiting_approval" || status == "paused" || status == "uncertain");
    Json result{{"resultType", "complete"},
                {"taskId", id},
                {"status", cancelled           ? "cancelled"
                           : success || failed ? "completed"
                           : input             ? "input_required"
                                               : "working"},
                {"createdAt", binding->data.at("created_at")},
                {"lastUpdatedAt", json_string(state, "updated_at", utc_now())},
                {"ttlMs", nullptr},
                {"pollIntervalMs", 1000},
                {"statusMessage", status},
                {"_meta", {{"devbox", {{"kind", binding->group}, {"handleId", handle}}}}}};
    if (success || failed) {
        auto value =
            failed ? result_error("The underlying operation did not succeed.", std::optional<Json>{state})
                   : result_explicit("The durable operation completed.", std::optional<Json>{state},
                                     state.dump());
        value["resultType"] = "complete";
        result["result"] = std::move(value);
    } else if (input) {
        std::string key, message;
        Json properties = Json::object();
        Json required = Json::array();
        if (status == "awaiting_approval") {
            key = json_string(state.at("pending_approval"), "operation_id");
            message = "Review this run's pending operation and issue its exact grant through the operator "
                      "CLI, then supply the grant ID. This form cannot create authority.";
            properties["grant_id"] = Json{{"type", "string"}};
            required.push_back("grant_id");
        } else if (status == "paused") {
            key = "resume";
            message = "Resume or cancel the paused run.";
            properties["action"] = Json{{"type", "string"}, {"enum", {"resume", "cancel"}}};
            required.push_back("action");
        } else {
            key = "reconcile";
            message =
                "The outcome is uncertain. Abandon it, or discard an unknown model reply while retaining its "
                "reserved cost. Use the run tool for a documented operator effect observation.";
            properties["resolution"] = Json{{"type", "string"}, {"enum", {"abandon", "discard_model_reply"}}};
            required.push_back("resolution");
        }
        result["inputRequests"] =
            Json{{key,
                  {{"method", "elicitation/create"},
                   {"params",
                    {{"mode", "form"},
                     {"message", message},
                     {"requestedSchema",
                      {{"type", "object"}, {"properties", properties}, {"required", required}}}}}}}};
    }
    if (success || failed || cancelled) {
        auto record = *binding;
        record.status = json_string(result, "status");
        record.data["terminal_result"] = result;
        const StateMutation terminal{record, record.revision};
        try {
            store->apply({&terminal, 1});
        } catch (...) {
            const auto raced = store->get("mcp_task", id);
            if (raced && raced->principal == principal && raced->data.contains("terminal_result"))
                return raced->data["terminal_result"];
            throw;
        }
    }
    return result;
}
Json TaskService::call(std::string_view principal, std::string_view method, const Json& params) {
    const auto id = json_string(params, "taskId");
    auto current = detail(principal, id);
    if (method == "tasks/get")
        return current;
    const auto binding = jobs_.index()->get("mcp_task", id);
    if (!binding || binding->principal != principal)
        throw Error("TASK_NOT_FOUND");
    const auto handle = json_string(binding->data, "handle");
    if (method == "tasks/cancel") {
        if (!binding->data.contains("terminal_result")) {
            if (binding->group == "run")
                (void)RunService(config_).call(principal, Json{{"action", "cancel"}, {"run_id", handle}});
            else
                (void)jobs_.cancel(handle);
        }
    } else if (method == "tasks/update") {
        const auto responses = params.value("inputResponses", Json::object());
        if (!responses.is_object())
            throw Error("TASK_INPUT_RESPONSES_REQUIRED");
        for (auto item = responses.begin(); item != responses.end(); ++item) {
            if (!current.contains("inputRequests") || !current["inputRequests"].contains(item.key()))
                continue;
            const auto action = json_string(item.value(), "action");
            if (action == "cancel" || action == "decline") {
                (void)RunService(config_).call(principal, Json{{"action", "cancel"}, {"run_id", handle}});
            } else if (action == "accept") {
                const auto content = item.value().value("content", Json::object());
                Json command{{"run_id", handle}};
                if (item.key() == "resume")
                    command["action"] = json_string(content, "action");
                else if (item.key() == "reconcile") {
                    command["action"] = "reconcile";
                    command["resolution"] = json_string(content, "resolution");
                } else {
                    command["action"] = "approve";
                    command["grant_id"] = json_string(content, "grant_id");
                }
                (void)RunService(config_).call(principal, command);
            } else
                throw Error("TASK_INPUT_ACTION_INVALID");
            current = detail(principal, id);
        }
    } else
        throw Error("TASK_METHOD_UNKNOWN");
    return Json{{"resultType", "complete"}};
}
} // namespace devbox
