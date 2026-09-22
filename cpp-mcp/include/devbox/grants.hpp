#pragma once
#include "isolation.hpp"
#include "security.hpp"
#include "state_store.hpp"
namespace devbox {
struct GrantContext {
    std::string principal, run, operation;
};
struct GrantDefinition {
    GrantContext context;
    std::string tool;
    Json arguments = Json::object();
    fs::path workspace, executable;
    std::optional<std::string> executable_sha256;
    std::vector<std::string> egress_origins;
    std::uint64_t expires_at_ms = 0;
};
struct GrantOptions {
    // Test clock only; production callers use unix_millis. Not exposed to model tool arguments.
    std::function<std::uint64_t()> now = unix_millis;
    std::uint64_t audit_max_bytes = 64 * 1024 * 1024;
};
class GrantAuthority {
    std::shared_ptr<StateStore> state_;
    fs::path root_;
    GrantOptions options_;
    SecurityAudit audit_;

  public:
    GrantAuthority(std::shared_ptr<StateStore> state, fs::path private_root, GrantOptions options = {});
    // Operator-only entrypoints. Do not register issue/revoke as model-callable MCP tools.
    std::string issue(const GrantDefinition& definition);
    void revoke(std::string_view grant_id);
    Json inspect(std::string_view grant_id) const;
    // Admission is the revocation boundary. An already admitted effect needs run cancellation,
    // not retroactive permission removal. The durable operation identity can be consumed only once.
    Json perform(std::string_view grant_id, const GrantContext& context, std::string_view tool,
                 const Json& arguments, const std::function<Json(const GrantDefinition&)>& effect);
};
GrantDefinition grant_definition(const Json& value);
Json execute_granted_program(GrantAuthority& authority, const fs::path& private_root,
                             std::string_view grant_id, const GrantContext& context, const Json& arguments,
                             const Cancel& cancel = {});
} // namespace devbox
