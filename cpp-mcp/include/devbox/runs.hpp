#pragma once
#include "grants.hpp"
#include "provider.hpp"
namespace devbox {
struct RunBudget {
    std::uint64_t rounds = 16, tool_calls = 32, repetitions = 3, tokens = 200000, cost_micro_usd = 0;
    std::uint64_t call_tokens = 32768, call_cost_micro_usd = 0, output_tokens = 4096;
    std::uint64_t wall_ms = 300000;
    std::size_t context_bytes = 65536, artifact_bytes = 4 * 1024 * 1024;
};
struct RunSpec {
    std::string principal, request_id, goal, provider_id, provider_fingerprint, tool_schema_sha256;
    Json tools = Json::array();
    RunBudget budget;
};
struct RunHooks {
    // Native crash injection and qualified broker callbacks. No model can supply these functions.
    std::function<void(std::string_view)> transition;
    std::function<ProviderResult(const ProviderRequest&)> model;
    std::function<Json(const GrantDefinition&)> tool;
};
class RunController {
    std::shared_ptr<StateStore> state_;
    fs::path root_;
    GrantAuthority& grants_;
    StateRecord read(std::string_view principal, std::string_view id) const;
    void save(StateRecord& record, std::string_view event);
    Json artifact(StateRecord& record, std::string_view kind, const Json& data);
    Json context(const StateRecord& record) const;
    void context(StateRecord& record, const Json& messages);

  public:
    RunController(std::shared_ptr<StateStore> state, fs::path private_root, GrantAuthority& grants);
    Json create(const RunSpec& spec);
    Json get(std::string_view principal, std::string_view id) const;
    Json events(std::string_view principal, std::string_view id, std::uint64_t after,
                std::size_t limit) const;
    Json control(std::string_view principal, std::string_view id, std::string_view action);
    Json approve(std::string_view principal, std::string_view id, std::string_view grant_id);
    // Reconciliation is operator-only and cannot be invoked as a model tool. A completed grant
    // receipt can recover an effect automatically; unknown effects require explicit resolution.
    Json reconcile(std::string_view principal, std::string_view id, std::string_view resolution,
                   const Json& observed_result = Json::object());
    Json step(std::string_view principal, std::string_view id, const RunHooks& hooks,
              const Cancel& cancel = {});
    Json read_artifact(std::string_view principal, std::string_view id, std::string_view sha256) const;
};
} // namespace devbox
