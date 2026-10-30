#pragma once
#include "process.hpp"
namespace devbox {
// Supplied only by a compiled offline installer after validating its embedded
// payload identity. There is no CLI/MCP switch accepting this trust assertion.
// The installer itself is a release artifact with its own GitHub attestation.
struct BundledQualification {
    std::string source, tree, target, sha256, workflow_run;
    std::uint64_t bytes = 0;
    fs::path contents;
};
// Operator CLI only. These operations are never exposed as model-facing MCP tools.
struct ManagementOptions {
    std::string command = "help", service = "systemd";
    fs::path root, binary, receipt, bundle, env_file, output, companions;
    std::string source, target;
    unsigned port = 8100;
    Millis timeout{30000};
    bool allow_local_build = false, port_set = false, timeout_set = false;
    const BundledQualification* bundled = nullptr;
};
using ManagementRunner =
    std::function<ProcessOutput(const fs::path&, const std::vector<std::string>&, const ProcessOptions&)>;
ManagementOptions parse_management_options(const std::vector<std::string>& args);
std::uint64_t management_instance(const Json& identity);
void validate_managed_handoff_selection(const Json& current, const Json& handoff);
Environment managed_environment(const fs::path& root, const Json& config, std::string generation = {});
Json qualify_managed_binary(const ManagementOptions& options, const Environment& env,
                            const ManagementRunner& runner = {});
std::string native_service_definition(std::string_view kind, const fs::path& binary, const fs::path& root);
struct ServiceMemoryPlan {
    bool unified = false;
    std::string group;
    std::uint64_t physical_bytes = 0, total_bytes = 0;
};
ServiceMemoryPlan service_memory_plan(std::string_view membership, std::string_view unit,
                                      std::uint64_t physical_bytes, std::uint64_t total_bytes);
// Operator-only startup check. Limit writes require an otherwise empty system
// service cgroup; recovery may re-verify existing limits without changing them.
// Never exposed as an MCP tool; it cannot select an arbitrary cgroup or raise limits.
Json apply_service_memory_guard(std::string_view unit, std::uint64_t physical_bytes,
                                std::uint64_t total_bytes);
void validate_managed_computer_selection(const fs::path& root, const Json& config, const Json& build,
                                         std::string_view hash, std::string_view source,
                                         std::string_view pipe);
int run_managed_computer_broker(const fs::path& root, std::string_view hash, std::string_view source,
                                std::string_view pipe, const std::function<bool()>& stopping);
int management_main(const std::vector<std::string>& args, const BundledQualification* bundled = nullptr);
} // namespace devbox
