#include "devbox/runs.hpp"
#include <functional>
#include <iostream>
#include <utility>
using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
// Inject at the transaction boundary, before SQLite checks the expected revision.
// The competing controls still commit through the real store, not a fake CAS result.
class ControlStore final : public StateStore {
    std::shared_ptr<StateStore> inner_;

  public:
    std::function<void()> before_apply;
    explicit ControlStore(std::shared_ptr<StateStore> inner) : inner_(std::move(inner)) {}
    std::optional<StateRecord> get(std::string_view kind, std::string_view id) const override {
        return inner_->get(kind, id);
    }
    StatePage list(const StateQuery& query) const override {
        return inner_->list(query);
    }
    std::uint64_t count(std::string_view kind, const std::optional<std::string>& principal,
                        const std::optional<std::string>& status) const override {
        return inner_->count(kind, principal, status);
    }
    std::uint64_t count_matching(const StateCountQuery& query) const override {
        return inner_->count_matching(query);
    }
    void apply(std::span<const StateMutation> mutations, std::span<const StateEvent> events = {}) override {
        if (auto inject = std::exchange(before_apply, {}))
            inject();
        inner_->apply(mutations, events);
    }
    bool apply_once(std::string_view id, std::span<const StateMutation> mutations,
                    std::span<const StateEvent> events) override {
        return inner_->apply_once(id, mutations, events);
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
    Json private_snapshot(const fs::path& destination) override {
        return inner_->private_snapshot(destination);
    }
};
struct Fixture {
    fs::path root;
    std::shared_ptr<ControlStore> state;
    GrantAuthority grants;
    RunController controller;
    std::string id;
    RunHooks hooks;
    unsigned models = 0, effects = 0;
    explicit Fixture(const fs::path& path)
        : root(path), state(std::make_shared<ControlStore>(open_state_store(path / "state"))),
          grants(state, path / "grants"), controller(state, path / "runs", grants) {
        RunSpec spec;
        spec.principal = "operator";
        spec.request_id = "request";
        spec.provider_id = "fixture";
        spec.budget.cost_micro_usd = 10000;
        spec.budget.call_cost_micro_usd = 100;
        spec.goal = "Perform only the exact approved operation.";
        spec.provider_fingerprint = sha256("fixture");
        spec.tools = Json::array({Json{
            {"type", "function"},
            {"function",
             {{"name", "note"},
              {"parameters", {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}}}}}}});
        spec.tool_schema_sha256 = sha256(spec.tools.dump());
        id = json_string(controller.create(spec), "run_id");
        hooks.model = [&](const ProviderRequest&) {
            ++models;
            return reply(true);
        };
        hooks.tool = [&](const GrantDefinition&) {
            ++effects;
            return Json{{"ok", true}};
        };
    }
    static ProviderResult reply(bool tool) {
        ProviderResult result;
        result.status = "completed";
        result.billing_unknown = false;
        result.usage.input_tokens = 10;
        result.usage.output_tokens = 5;
        result.usage.cost_ceiling_micro_usd = 0;
        if (tool)
            result.calls.push_back({"call_one", "note", Json{{"text", "approved"}}});
        else
            result.text = "Observed result.";
        return result;
    }
    Json step() {
        return controller.step("operator", id, hooks);
    }
    Json get() {
        return controller.get("operator", id);
    }
    Json control(std::string_view action) {
        return controller.control("operator", id, action);
    }
    void approval_wait() {
        require(step()["phase"] == "tool_gate", "proposal persisted");
        require(step()["status"] == "awaiting_approval", "explicit approval gate");
    }
    void approve() {
        const auto pending = get().at("pending_approval");
        GrantDefinition grant;
        grant.context = {"operator", id, json_string(pending, "operation_id")};
        grant.tool = "note";
        grant.arguments = pending.at("arguments");
        grant.expires_at_ms = unix_millis() + 60000;
        controller.approve("operator", id, grants.issue(grant));
    }
    void expire() {
        auto record = *state->get("run", id);
        record.data["expires_at_ms"] = 1;
        StateMutation update{record, record.revision};
        state->apply({&update, 1});
    }
    void interrupted(std::string_view boundary) {
        hooks.transition = [&](std::string_view point) {
            if (point == boundary)
                throw Error("fixture_crash");
        };
        bool observed = false;
        try {
            step();
        } catch (const Error& error) {
            observed = std::string_view(error.what()) == "fixture_crash";
        }
        hooks.transition = {};
        require(observed, "crash injected at durable transition");
    }
};
void approval_resume(Fixture& f) {
    f.approval_wait();
    const auto pending = f.get()["pending_approval"];
    f.control("pause");
    const auto resumed = f.control("resume");
    require(resumed["status"] == "awaiting_approval", "resume must restore the approval wait");
    require(resumed["pending_approval"] == pending, "resume preserves exact pending operation");
    for (unsigned i = 0; i < 4; ++i)
        require(f.step()["status"] == "awaiting_approval", "approval wait does not spin as ready");
    require(f.models == 1 && f.effects == 0, "resume grants no execution authority");
    f.approve();
    f.step();
    require(f.effects == 1, "original operation remains approvable exactly once");
}
void final_reply(Fixture& f, bool cancel) {
    f.hooks.model = [&](const ProviderRequest&) {
        ++f.models;
        require(f.control(cancel ? "cancel" : "pause")["terminal_acknowledgement"] == false,
                "in-flight control acknowledgement is not terminal");
        return Fixture::reply(false);
    };
    const auto result = f.step();
    require(result["status"] == (cancel ? "cancelled" : "completed"),
            "cancellation wins final-reply commit; pause lets the current final step complete");
    require(result["tokens"] == 15 && !result["artifacts"].empty(), "completed reply remains accounted");
    require(f.step()["status"] == result["status"] && f.models == 1 && f.effects == 0,
            "terminal result never dispatches again");
    require(f.controller.events("operator", f.id, 0, 100).back()["type"] ==
                (cancel ? "cancelled" : "completed"),
            "terminal event agrees with settled status");
}
void before_dispatch(Fixture& f, bool tool, std::string action) {
    if (tool) {
        f.approval_wait();
        f.approve();
    }
    f.hooks.transition = [&](std::string_view point) {
        if (point == (tool ? "tool_admitted" : "model_admitted")) {
            if (action == "expire")
                f.expire();
            else
                f.control(action);
        }
    };
    const auto result = f.step();
    const auto expected = action == "expire" ? "failed" : action == "pause" ? "paused" : "cancelled";
    require(result["status"] == expected, "observed stop wins before external dispatch");
    require(f.effects == 0 && f.models == (tool ? 1U : 0U), "stopped admission performs no external call");
    require(result["tool_calls"] == 0 && result["rounds"] == (tool ? 1 : 0) &&
                result["tokens"] == (tool ? 15 : 0),
            "known-undispatched reservation is refunded");
    require(!json_bool(result, "external_outcome_unknown"), "known non-dispatch is not uncertain");
    require(result["cost_micro_usd"] == 0, "known non-dispatch refunds the nonzero monetary reservation");
    if (action == "pause") {
        f.hooks.transition = {};
        f.control("resume");
        f.step();
        require(tool ? f.effects == 1 : f.models == 1, "resume dispatches the original step only once");
    }
}
void pending_stop(Fixture& f, bool tool, std::string action) {
    if (tool) {
        f.approval_wait();
        f.approve();
    }
    f.interrupted(tool ? "tool_admitted" : "model_admitted");
    const auto charged = f.get()["tokens"];
    const auto charged_cost = f.get()["cost_micro_usd"];
    if (action == "expire")
        f.expire();
    else
        f.control(action);
    const auto result = f.step();
    const auto expected = action == "pause" ? "uncertain" : action == "cancel" ? "cancelled" : "failed";
    require(result["status"] == expected, "stopping recovered pending work preserves its uncertainty");
    require(json_bool(result, "external_outcome_unknown"), "pending stop must expose unknown outcome");
    require(result["tokens"] == charged && f.effects == 0 && f.models == (tool ? 1U : 0U),
            "unknown dispatch retains its reservation and never repeats an effect");
    require(result["cost_micro_usd"] == charged_cost, "unknown dispatch retains monetary reservation");
    if (action == "pause") {
        bool denied = false;
        try {
            f.control("resume");
        } catch (const Error& error) {
            denied = std::string_view(error.what()).find("RECONCILIATION_REQUIRED") != std::string_view::npos;
        }
        require(denied, "pause cannot bypass pending-outcome reconciliation");
        require(f.step()["status"] == "uncertain", "repeated observation remains uncertain");
    }
}
void throwing_cancel(Fixture& f) {
    f.hooks.model = [&](const ProviderRequest&) -> ProviderResult {
        ++f.models;
        f.control("cancel");
        throw Error("lost_provider_acknowledgement");
    };
    const auto result = f.step();
    require(result["status"] == "cancelled" && json_bool(result, "external_outcome_unknown"),
            "cancelled throwing provider retains unknown outcome without dropping control");
    require(result["tokens"] == result["budget"]["call_tokens"], "unknown provider charge stays reserved");
}
void sticky_cancel(Fixture& f) {
    f.hooks.model = [&](const ProviderRequest&) {
        ++f.models;
        f.control("cancel");
        f.control("pause");
        return Fixture::reply(true);
    };
    f.step();
    require(f.step()["status"] == "cancelled" && f.effects == 0,
            "later pause cannot downgrade accepted cancellation");
}
void cancel_event(Fixture& f) {
    const auto result = f.control("cancel");
    require(result["terminal_acknowledgement"] == true, "idle cancellation is terminal");
    require(f.controller.events("operator", f.id, 0, 100).back()["data"]["terminal"] == true,
            "event terminal metadata agrees with cancellation acknowledgement");
}
void completed_stays_terminal(Fixture& f) {
    f.hooks.model = [&](const ProviderRequest&) {
        ++f.models;
        return Fixture::reply(false);
    };
    const auto completed = f.step();
    require(f.control("cancel")["revision"] == completed["revision"],
            "late cancellation does not rewrite completion");
    require(f.step()["status"] == "completed" && f.models == 1, "terminal replay is read only");
}
void completed_effect_replay(Fixture& f) {
    f.approval_wait();
    f.approve();
    f.interrupted("tool_returned");
    require(f.effects == 1, "effect finished before simulated lost reply");
    require(f.step()["phase"] == "tool_gate" && f.effects == 1,
            "existing durable receipt recovers without repeating the effect");
}
void paused_approval_denied(Fixture& f) {
    f.approval_wait();
    f.control("pause");
    bool denied = false;
    try {
        f.approve();
    } catch (const Error& error) {
        denied = std::string_view(error.what()).find("NOT_AWAITING_APPROVAL") != std::string_view::npos;
    }
    require(denied && f.effects == 0, "approval cannot dispatch a paused run");
}
void cancel_final_cas(Fixture& f) {
    f.hooks.model = [&](const ProviderRequest&) {
        ++f.models;
        return Fixture::reply(false);
    };
    f.hooks.transition = [&](std::string_view point) {
        if (point == "model_returned")
            f.state->before_apply = [&] { f.control("cancel"); };
    };
    const auto result = f.step();
    require(result["status"] == "cancelled" && result["tokens"] == 15 && f.models == 1,
            "final commit retries merge cancellation without repeating the model");
}
void sticky_control_cas(Fixture& f) {
    f.hooks.model = [&](const ProviderRequest&) {
        ++f.models;
        f.state->before_apply = [&] { f.control("cancel"); };
        const auto paused = f.control("pause");
        require(paused["control"] == "cancel", "stale pause cannot overwrite a competing cancellation");
        return Fixture::reply(false);
    };
    require(f.step()["status"] == "cancelled" && f.models == 1, "control CAS retry preserves cancellation");
}
void resume_during_approval_cas(Fixture& f) {
    f.approval_wait();
    const auto pending = f.get().at("pending_approval");
    GrantDefinition grant;
    grant.context = {"operator", f.id, json_string(pending, "operation_id")};
    grant.tool = "note";
    grant.arguments = pending.at("arguments");
    grant.expires_at_ms = unix_millis() + 60000;
    const auto grant_id = f.grants.issue(grant);
    f.state->before_apply = [&] {
        f.control("pause");
        f.state->before_apply = [&] { f.control("resume"); };
    };
    const auto approved = f.controller.approve("operator", f.id, grant_id);
    require(approved["status"] == "ready" && approved["control"] == "",
            "uncommitted pause suppression does not survive the newer resume");
    require(approved["pending_approval"] == pending, "approval conflict preserves exact operation identity");
    f.step();
    require(f.effects == 1, "approved operation executes exactly once after the conflict");
}
void bounded_cas_retries(Fixture& f) {
    unsigned attempts = 0;
    std::function<void()> conflict;
    conflict = [&] {
        ++attempts;
        f.state->before_apply = conflict;
        throw Error("STATE_REVISION_CONFLICT");
    };
    f.state->before_apply = conflict;
    bool stopped = false;
    try {
        f.step();
    } catch (const Error& error) {
        stopped = std::string_view(error.what()) == "RUN_STATE_CONTENTION";
    }
    f.state->before_apply = {};
    require(stopped && attempts == 8, "revision retries are explicitly bounded");
    require(f.models == 0 && f.get()["tokens"] == 0, "failed admission does not dispatch or charge");
    f.step();
    require(f.models == 1, "a later invocation can make progress without duplicated dispatch");
}
void cancel_admission_cas(Fixture& f) {
    f.state->before_apply = [&] { f.control("cancel"); };
    const auto result = f.step();
    require(result["status"] == "cancelled" && result["tokens"] == 0 && result["rounds"] == 0,
            "committed terminal control wins the admission CAS without a refund underflow");
    require(f.models == 0 && f.effects == 0, "terminal admission conflict never dispatches");
}
int run() {
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-run-controls-" + uuid());
    ensure_private_state_directory(root);
    std::vector<std::pair<std::string, std::function<void(Fixture&)>>> cases{
        {"approval_resume", approval_resume},
        {"cancel_final_cas", cancel_final_cas},
        {"sticky_control_cas", sticky_control_cas},
        {"resume_during_approval_cas", resume_during_approval_cas},
        {"bounded_cas_retries", bounded_cas_retries},
        {"cancel_admission_cas", cancel_admission_cas},
        {"cancel_final_reply", [](auto& f) { final_reply(f, true); }},
        {"pause_final_reply", [](auto& f) { final_reply(f, false); }},
        {"cancel_throwing_model", throwing_cancel},
        {"cancel_is_sticky", sticky_cancel},
        {"cancel_event_terminal", cancel_event},
        {"completed_stays_terminal", completed_stays_terminal},
        {"completed_effect_replay", completed_effect_replay},
        {"paused_approval_denied", paused_approval_denied}};
    for (bool tool : {false, true})
        for (const std::string action : {"cancel", "pause", "expire"}) {
            const std::string kind = tool ? "tool" : "model";
            cases.emplace_back(action + "_before_" + kind + "_dispatch",
                               [=](auto& f) { before_dispatch(f, tool, action); });
            cases.emplace_back(action + "_recovered_" + kind + "_pending",
                               [=](auto& f) { pending_stop(f, tool, action); });
        }
    Json report{
        {"suite", "harness-control-boundaries"}, {"cases", Json::array()}, {"passed", 0}, {"failed", 0}};
    for (const auto& [name, test] : cases) {
        ensure_private_state_directory(root / name);
        try {
            Fixture fixture(root / name);
            test(fixture);
            report["cases"].push_back(Json{{"name", name}, {"passed", true}});
            report["passed"] = json_uint(report, "passed") + 1;
        } catch (const std::exception& error) {
            report["cases"].push_back(Json{{"name", name}, {"passed", false}, {"error", error.what()}});
            report["failed"] = json_uint(report, "failed") + 1;
        }
    }
    std::cout << report.dump(2) << '\n';
    const bool failed = json_uint(report, "failed") != 0;
    if (failed)
        std::cerr << "Fixture: " << path_text(root) << '\n';
    else
        fs::remove_all(root);
    return failed ? 1 : 0;
}
} // namespace
#ifdef _WIN32
int wmain() {
    return run();
}
#else
int main() {
    return run();
}
#endif
