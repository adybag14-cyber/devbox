#include "devbox/filesystem_worker.hpp"
#include "devbox/run_service.hpp"
#include <algorithm>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
std::string event(const Json& value) {
    return "data: " + value.dump() + "\n\n";
}
Json chunk(const Json& delta, const Json& reason = Json()) {
    return Json{{"id", "chat_reliability"},
                {"choices", Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}})}};
}
Json call(unsigned index) {
    return Json{{"index", index},
                {"id", "call_" + std::to_string(index)},
                {"type", "function"},
                {"function", {{"name", "program"}, {"arguments", "{\"args\":[]}"}}}};
}
void stream_order() {
    for (const unsigned count : {1U, 12U, 32U}) {
        ProviderStream stream(ProviderProtocol::ChatCompletions);
        // Arrival order is deliberately different from the provider's numeric call order.
        for (unsigned i = count; i > 0; --i) {
            const auto bytes = event(chunk(Json{{"tool_calls", Json::array({call(i - 1)})}}));
            for (const auto byte : bytes)
                stream.feed(std::string_view(&byte, 1));
        }
        stream.feed(event(chunk(Json::object(), "tool_calls")) + "data: [DONE]\n\n");
        const auto result = stream.finish();
        require(result.status == "completed" && result.calls.size() == count, "complete call batch");
        for (unsigned i = 0; i < count; ++i)
            require(result.calls[i].id == "call_" + std::to_string(i),
                    "streamed calls must retain numeric index order, including index 10");
        require(stream.finish().calls.size() == count, "successful finish remains idempotent");
    }
}
void stream_poison(ProviderProtocol protocol) {
    ProviderStream stream(protocol);
    if (protocol == ProviderProtocol::ChatCompletions) {
        stream.feed(event(chunk(Json{{"tool_calls", Json::array({call(0)})}}, "tool_calls")) +
                    "data: [DONE]\n\n");
    } else {
        stream.feed(event(Json{{"type", "response.completed"},
                               {"response",
                                {{"id", "response_reliability"},
                                 {"status", "completed"},
                                 {"output", Json::array({Json{{"type", "function_call"},
                                                              {"call_id", "call_one"},
                                                              {"name", "program"},
                                                              {"arguments", "{}"}}})}}}}));
    }
    bool rejected = false;
    try {
        stream.feed(event(Json{{"type", "error"}}));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "trailing protocol violation must be rejected");
    const auto result = stream.finish();
    require(result.status == "protocol_error" && result.calls.empty() && result.native_output.empty() &&
                result.billing_unknown,
            "a rejected stream must never expose an earlier executable batch");
    require(stream.finish().calls.empty(), "poisoned finish remains non-executable on repeated reads");
}
void chat_error_before_done(bool typed) {
    ProviderStream stream(ProviderProtocol::ChatCompletions);
    stream.feed(event(chunk(Json{{"tool_calls", Json::array({call(0)})}}, "tool_calls")));
    bool rejected = false;
    try {
        stream.feed(event(typed ? Json{{"type", "error"}, {"message", "recorded failure"}} :
                                  Json{{"error", {{"message", "recorded failure"}}}}));
        stream.feed("data: [DONE]\n\n");
    } catch (const std::exception&) { rejected = true; }
    const auto result = stream.finish();
    require(rejected && result.status == "protocol_error" && result.calls.empty() &&
                result.native_output.empty(),
            "an error envelope before DONE must invalidate previously completed calls");
}
void response_unfinished_tail() {
    ProviderStream stream(ProviderProtocol::Responses);
    stream.feed(event(Json{{"type", "response.completed"},
                           {"response", {{"id", "response_tail"}, {"status", "completed"},
                                         {"output", Json::array({Json{{"type", "function_call"},
                                                                      {"call_id", "call_tail"},
                                                                      {"name", "program"},
                                                                      {"arguments", "{}"}}})}}}}));
    stream.feed("data: {\"type\":");
    const auto result = stream.finish();
    require(result.status == "interrupted" && result.calls.empty() && result.native_output.empty() &&
                result.text.empty() && result.billing_unknown,
            "an unfinished trailing event must clear all executable native output");
    require(stream.finish().json() == result.json(), "interrupted finish must remain idempotent");
}
void chat_nullable_fragments() {
    ProviderStream stream(ProviderProtocol::ChatCompletions);
    auto first = call(0);
    first["function"]["arguments"] = "{\"args\":";
    stream.feed(event(chunk(Json{{"role", "assistant"}, {"tool_calls", Json::array({first})}})));
    auto next = call(0);
    next["id"] = next["type"] = next["function"]["name"] = nullptr;
    next["function"]["arguments"] = "[]}";
    stream.feed(event(chunk(Json{{"role", nullptr}, {"tool_calls", Json::array({next})}}, "tool_calls")));
    stream.feed("data: [DONE]\n\n");
    const auto result = stream.finish();
    require(result.status == "completed" && result.calls.size() == 1 &&
                result.calls[0].arguments.at("args").is_array(),
            "nullable incremental metadata must remain compatible with valid provider streams");
    bool rejected = false;
    try {
        stream.feed("data: unexpected\n\n");
    } catch (const Error&) {
        rejected = true;
    }
    require(rejected && stream.finish().json() == result.json(),
            "feed after successful finalization cannot change the committed parser result");
}
void chat_shape(std::string_view fault) {
    ProviderStream stream(ProviderProtocol::ChatCompletions);
    auto tool = call(0);
    Json delta{{"tool_calls", Json::array({tool})}};
    if (fault == "role")
        delta["role"] = "system";
    else if (fault == "type")
        delta["tool_calls"][0]["type"] = "unapproved_tool_type";
    else if (fault == "index")
        delta["tool_calls"][0].erase("index");
    bool rejected = false;
    try {
        stream.feed(event(chunk(delta, "tool_calls")) + "data: [DONE]\n\n");
        (void)stream.finish();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "malformed streamed role, tool type or missing index must be rejected");
    require(stream.finish().calls.empty(), "malformed streams cannot yield calls after rejection");
}
RunSpec spec(std::string request = "reliability") {
    RunSpec value;
    value.principal = "operator";
    value.request_id = std::move(request);
    value.provider_id = "recorded";
    value.provider_fingerprint = sha256("fixture-provider");
    value.tool_schema_sha256 = sha256("fixture-tools");
    value.goal = "Exercise only the owned reliability fixture.";
    return value;
}
ProviderResult answer() {
    ProviderResult result;
    result.status = "completed";
    result.text = "Recorded fixture answer.";
    result.billing_unknown = false;
    result.usage.input_tokens = 10;
    result.usage.output_tokens = 5;
    result.usage.cost_ceiling_micro_usd = 0;
    return result;
}
void cancellation(const fs::path& root) {
    auto store = open_state_store(root / "state");
    GrantAuthority grants(store, root / "grants");
    RunController controller(store, root / "runs", grants);
    const auto id = json_string(controller.create(spec()), "run_id");
    bool cancel_was_pending = false, preserved = false;
    unsigned generations = 0;
    RunHooks hooks;
    hooks.model = [&](const ProviderRequest&) {
        ++generations;
        const auto cancelled = controller.control("operator", id, "cancel");
        cancel_was_pending = !json_bool(cancelled, "terminal_acknowledgement");
        const auto paused = controller.control("operator", id, "pause");
        preserved = json_string(paused, "control") == "cancel";
        // A non-completed outcome exercises the existing cancellation resolution path.
        auto result = answer();
        result.status = "cancel_requested";
        result.billing_unknown = true;
        return result;
    };
    const auto result = controller.step("operator", id, hooks);
    require(cancel_was_pending && preserved, "an accepted cancellation cannot be weakened into pause");
    require(result["status"] == "cancelled" && result["external_outcome_unknown"] == true,
            "cancellation must retain unknown external outcome without admitting more work");
    require(controller.step("operator", id, hooks)["status"] == "cancelled" && generations == 1,
            "terminal cancellation cannot regenerate");
}
// Fault-injection wrapper delegates every storage operation to the real SQLite
// implementation. Only an explicitly selected read interleaving is intercepted.
class InterleavedStore final : public StateStore {
    std::shared_ptr<StateStore> inner_;

  public:
    mutable std::function<void(std::string_view)> before_get;
    explicit InterleavedStore(std::shared_ptr<StateStore> inner) : inner_(std::move(inner)) {}
    std::optional<StateRecord> get(std::string_view kind, std::string_view id) const override {
        if (before_get) {
            auto hook = before_get;
            hook(kind);
        }
        return inner_->get(kind, id);
    }
    StatePage list(const StateQuery& q) const override {
        return inner_->list(q);
    }
    std::uint64_t count(std::string_view kind, const std::optional<std::string>& principal,
                        const std::optional<std::string>& status) const override {
        return inner_->count(kind, principal, status);
    }
    std::uint64_t count_matching(const StateCountQuery& q) const override {
        return inner_->count_matching(q);
    }
    void apply(std::span<const StateMutation> changes, std::span<const StateEvent> events = {}) override {
        inner_->apply(changes, events);
    }
    bool apply_once(std::string_view id, std::span<const StateMutation> changes,
                    std::span<const StateEvent> events = {}) override {
        return inner_->apply_once(id, changes, events);
    }
    void import_records(std::span<const StateRecord> records) override {
        inner_->import_records(records);
    }
    std::vector<StateEvent> events(std::string_view run, std::uint64_t after,
                                   std::size_t limit) const override {
        return inner_->events(run, after, limit);
    }
    std::uint64_t generation() const override {
        return inner_->generation();
    }
    void release_writer() override {
        inner_->release_writer();
    }
    Json diagnostics() const override {
        return inner_->diagnostics();
    }
    Json private_snapshot(const fs::path& target) override {
        return inner_->private_snapshot(target);
    }
};
void admission_cancellation(const fs::path& root, bool tool, std::string_view command) {
    auto store = std::make_shared<InterleavedStore>(open_state_store(root / "state"));
    GrantAuthority grants(store, root / "grants");
    RunController controller(store, root / "runs", grants);
    auto definition = spec();
    definition.tools = Json::array({Json{{"type", "function"}, {"function", {{"name", "write_note"}}}}});
    const auto id = json_string(controller.create(definition), "run_id");
    unsigned generations = 0, effects = 0;
    RunHooks hooks;
    hooks.model = [&](const ProviderRequest&) {
        ++generations;
        auto reply = answer();
        if (tool)
            reply.calls.push_back({"call_one", "write_note", Json{{"text", "fixture"}}});
        return reply;
    };
    hooks.tool = [&](const GrantDefinition&) {
        ++effects;
        return Json{{"ok", true}};
    };
    if (tool) {
        (void)controller.step("operator", id, hooks);
        const auto pending = controller.step("operator", id, hooks).at("pending_approval");
        GrantDefinition grant;
        grant.context = {"operator", id, json_string(pending, "operation_id")};
        grant.tool = "write_note";
        grant.arguments = pending.at("arguments");
        grant.expires_at_ms = unix_millis() + 60000;
        (void)controller.approve("operator", id, grants.issue(grant));
    }
    unsigned reads = 0;
    const auto generations_before = generations;
    Json cancelled;
    store->before_get = [&](std::string_view kind) {
        // step reads before and after taking its lease, then save rechecks admission.
        if (kind == "run" && ++reads == 3) {
            store->before_get = {};
            cancelled = controller.control("operator", id, command);
        }
    };
    const auto result = controller.step("operator", id, hooks);
    require(!cancelled.is_null() && result["status"] == (command == "cancel" ? "cancelled" : "paused") &&
                result["revision"] == cancelled["revision"],
            "accepted control wins before admission without consuming a budget reservation");
    require(generations == generations_before && effects == 0,
            "losing admission must return before calling the provider or executing a granted effect");
}
void terminal_control_race(const fs::path& root, bool approval) {
    auto store = std::make_shared<InterleavedStore>(open_state_store(root / "state"));
    GrantAuthority grants(store, root / "grants");
    RunController controller(store, root / "runs", grants);
    auto definition = spec();
    definition.tools = Json::array({Json{{"type", "function"}, {"function", {{"name", "write_note"}}}}});
    const auto id = json_string(controller.create(definition), "run_id");
    RunHooks hooks;
    hooks.model = [=](const ProviderRequest&) -> ProviderResult {
        if (!approval)
            throw Error("Recorded unknown model outcome");
        auto result = answer();
        result.calls.push_back({"call_one", "write_note", Json{{"text", "fixture"}}});
        return result;
    };
    (void)controller.step("operator", id, hooks);
    std::string grant_id;
    if (approval) {
        const auto pending = controller.step("operator", id, hooks).at("pending_approval");
        GrantDefinition grant;
        grant.context = {"operator", id, json_string(pending, "operation_id")};
        grant.tool = "write_note";
        grant.arguments = pending.at("arguments");
        grant.expires_at_ms = unix_millis() + 60000;
        grant_id = grants.issue(grant);
    }
    unsigned reads = 0;
    Json cancelled;
    store->before_get = [&](std::string_view kind) {
        if (kind == "run" && ++reads == 2) {
            store->before_get = {};
            cancelled = controller.control("operator", id, "cancel");
        }
    };
    const auto result = approval ? controller.approve("operator", id, grant_id)
                                 : controller.reconcile("operator", id, "discard_model_reply");
    require(!cancelled.is_null() && cancelled["status"] == "cancelled", "the terminal interleaving occurred");
    require(result["status"] == "cancelled" && result["revision"] == cancelled["revision"],
            "late approval or reconciliation must not resurrect a terminal cancelled run");
    require(controller.events("operator", id, 0, 100).back()["type"] == "cancel_requested",
            "no approval or reconciliation event may be appended after terminal cancellation");
    require(controller.step("operator", id, hooks)["status"] == "cancelled", "no subsequent dispatch");
}
void driver(const fs::path& root, std::string_view scenario) {
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->state_backend = "sqlite";
    config->state_root = root / "state";
    config->jobs_root = root / "jobs";
    config->execution_slot_root = root / "slots";
    config->runtime_mode = RuntimeMode::host;
    config->platform = Platform::detect();
    config->host_exec_enabled = true;
    config->host_workspace_path = config->host_default_workdir = config->devbox_workspace_path = root;
    ScopeExit cleanup([&] { (void)stop_state_coordinator(config->state_root); });
    JobStore jobs(config);
    auto store = jobs.index();
    GrantAuthority grants(store, root / "state" / "grants");
    RunController controller(store, root / "state" / "runs", grants);
    const auto id = json_string(controller.create(spec()), "run_id");
    RunHooks hooks;
    hooks.model = [](const auto&) { return answer(); };
    if (scenario == "completed" || scenario == "resume-completed")
        (void)controller.step("operator", id, hooks);
    else if (scenario == "cancelled" || scenario == "resume-cancelled")
        (void)controller.control("operator", id, "cancel");
    else if (scenario == "failed" || scenario == "resume-failed") {
        auto record = *store->get("run", id);
        record.status = "failed";
        record.data["error_code"] = "ORIGINAL_FAILURE";
        const StateMutation update{record, record.revision};
        store->apply({&update, 1});
    } else if (scenario == "pending") {
        auto record = *store->get("run", id);
        record.status = "running";
        record.data["phase"] = "model_pending";
        const StateMutation update{record, record.revision};
        store->apply({&update, 1});
    }
    if (scenario == "paused" || scenario == "uncertain" || scenario == "awaiting_approval" ||
        scenario.starts_with("cancel-") || scenario.starts_with("pause-")) {
        auto record = *store->get("run", id);
        if (scenario == "paused" || scenario == "uncertain" || scenario == "awaiting_approval") {
            record.status = scenario;
            record.data["phase"] = scenario == "uncertain" ? "model_pending" :
                                   scenario == "awaiting_approval" ? "approval_wait" : "model_ready";
        } else {
            record.status = "running";
            record.data["phase"] = scenario.ends_with("pending") ? "model_pending" : "model_ready";
            record.data["control"] = scenario.starts_with("cancel-") ? "cancel" : "pause";
        }
        const StateMutation update{record, record.revision};
        store->apply({&update, 1});
    }
    const auto before = controller.get("operator", id);
    const auto events_before = controller.events("operator", id, 0, 100);
    RunService service(config);
    if (scenario.starts_with("resume-")) {
        const auto result = service.call("operator", Json{{"action", "resume"}, {"run_id", id}});
        require(result == before && !result.contains("driver"),
                "a terminal resume must not report or launch a driver");
        require(!fs::exists(root / "state" / "runs" / id / "driver-owner.json"),
                "a terminal resume must not leave driver ownership metadata");
    } else if (scenario == "contender") {
        FileLock owner(root / "state" / "runs" / id / ".driver.lock", Millis(1000), {}, true);
        require(service.drive("operator", id) != 0, "a competing driver must not acquire ownership");
    } else
        (void)service.drive("operator", id);
    const auto after = controller.get("operator", id);
    if (scenario == "ready") {
        require(after["status"] == "failed" && after["error_code"] == "RUN_DRIVER_FAILURE",
                "the owning driver must still record a real configuration failure");
    } else if (scenario == "pending") {
        require(after["status"] == "uncertain" && after["error_code"] == "RUN_DRIVER_FAILURE",
                "an admitted unknown outcome must stay uncertain, not become safe to retry");
    } else if (scenario.starts_with("cancel-") || scenario.starts_with("pause-")) {
        const bool pending = scenario.ends_with("pending");
        const bool cancelling = scenario.starts_with("cancel-");
        require(after["status"] == (cancelling ? "cancelled" : pending ? "uncertain" : "paused") &&
                    after["error_code"] == "RUN_DRIVER_FAILURE",
                "driver failure must preserve an accepted operator control request");
        require(json_bool(after, "external_outcome_unknown") == pending,
                "pending external outcomes must remain explicit through controlled driver failure");
    } else {
        require(after == before, "a non-owner or late driver failure must not rewrite run state or revision");
        require(controller.events("operator", id, 0, 100) == events_before,
                "a non-owner or late driver failure must not append a misleading failure event");
    }
}
void recovered_control(const fs::path& root, std::string_view command, std::string_view phase) {
    auto store = open_state_store(root / "state");
    GrantAuthority grants(store, root / "grants");
    RunController controller(store, root / "runs", grants);
    const auto id = json_string(controller.create(spec()), "run_id");
    auto record = *store->get("run", id);
    record.status = "running";
    record.data["phase"] = phase;
    record.data["control"] = command;
    const StateMutation update{record, record.revision};
    store->apply({&update, 1});
    unsigned callbacks = 0;
    RunHooks hooks;
    hooks.model = [&](const ProviderRequest&) { ++callbacks; return answer(); };
    hooks.tool = [&](const GrantDefinition&) { ++callbacks; return Json::object(); };
    const auto result = controller.step("operator", id, hooks);
    require(result["status"] == (command == "cancel" ? "cancelled" : "uncertain") &&
                json_bool(result, "external_outcome_unknown") && callbacks == 0,
            "recovery with control intent must retain unknown outcomes without dispatch");
    if (command == "pause") {
        bool denied = false;
        try { (void)controller.control("operator", id, "resume"); }
        catch (const Error&) { denied = true; }
        require(denied, "a paused unknown effect requires reconciliation, not blind resume");
    }
}
int run(int argc, char** argv) {
    // A negative-control service launch must never recursively run the test suite.
    if (argc == 4 && std::string_view(argv[1]) == "--agent-runner")
        return 0;
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker();
    if (argc == 3 && std::string_view(argv[1]) == "--state-coordinator")
        return run_state_coordinator(path_from_utf8(argv[2]), [] { return false; });
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-harness-reliability-" + uuid());
    ensure_private_state_directory(root);
    unsigned passed = 0, failed = 0;
    const auto check = [&](std::string_view name, const std::function<void()>& test) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    check("numeric-stream-call-order", stream_order);
    check("nullable-fragments-and-finalization", chat_nullable_fragments);
    check("chat-error-envelope-before-done", [] { chat_error_before_done(false); });
    check("chat-typed-error-before-done", [] { chat_error_before_done(true); });
    check("responses-unfinished-tail", response_unfinished_tail);
    check("chat-error-poisons-stream", [] { stream_poison(ProviderProtocol::ChatCompletions); });
    check("responses-error-poisons-stream", [] { stream_poison(ProviderProtocol::Responses); });
    for (const auto* fault : {"role", "type", "index"})
        check(std::string("chat-rejects-") + fault, [=] { chat_shape(fault); });
    ensure_directory(root / "cancellation");
    check("cancellation-is-monotonic", [&] { cancellation(root / "cancellation"); });
    for (const auto* command : {"cancel", "pause"}) {
        for (const bool tool : {false, true}) {
            const auto name = std::string(tool ? "tool-admission-" : "model-admission-") + command;
            ensure_directory(root / name);
            check(name, [&] { admission_cancellation(root / name, tool, command); });
        }
        for (const auto* phase : {"model_pending", "tool_pending"}) {
            const auto name = std::string("recovered-") + command + "-" + phase;
            ensure_directory(root / name);
            check(name, [&] { recovered_control(root / name, command, phase); });
        }
    }
    for (const bool approval : {true, false}) {
        const auto name = approval ? "approval-cancellation-race" : "reconciliation-cancellation-race";
        ensure_directory(root / name);
        check(name, [&] { terminal_control_race(root / name, approval); });
    }
    for (const auto* scenario : {"completed", "cancelled", "failed", "contender", "ready", "pending",
                                  "paused", "uncertain", "awaiting_approval", "cancel-ready", "cancel-pending",
                                  "pause-ready", "pause-pending", "resume-completed", "resume-cancelled",
                                  "resume-failed"}) {
        ensure_directory(root / scenario);
        check(std::string("driver-") + scenario, [&] { driver(root / scenario, scenario); });
    }
    std::cout << "Harness reliability: " << passed << " passed, " << failed << " failed\n";
    if (!failed)
        fs::remove_all(root);
    else
        std::cout << "Retained fixture: " << path_text(root) << '\n';
    return failed ? 1 : 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(narrow(argv[i]));
    std::vector<char*> values;
    for (auto& arg : args)
        values.push_back(arg.data());
    return run(argc, values.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
