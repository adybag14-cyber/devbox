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
Environment managed_environment(const fs::path& root, const Json& config, std::string generation = {});
Json qualify_managed_binary(const ManagementOptions& options, const Environment& env,
                            const ManagementRunner& runner = {});
std::string native_service_definition(std::string_view kind, const fs::path& binary, const fs::path& root);
int management_main(const std::vector<std::string>& args, const BundledQualification* bundled = nullptr);
} // namespace devbox
