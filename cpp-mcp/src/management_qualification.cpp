#include "devbox/contract.hpp"
#include "devbox/management.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#include <set>

namespace devbox {
namespace {
bool hex_id(std::string_view text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
} // namespace
Json qualify_managed_binary(const ManagementOptions& options, const Environment& env,
                            const ManagementRunner& injected) {
    const auto runner =
        injected ? injected : ManagementRunner([](const auto& file, const auto& args, const auto& process) {
            return spawn_process(path_text(file), args, process);
        });
    const auto binary = fs::canonical(options.binary);
    const auto hash = sha256_file(binary);
    ProcessOptions process;
    process.cwd = options.root;
    process.env = env;
    process.timeout = Millis(30000);
    process.max_capture_chars = 2 * 1024 * 1024;
    const auto run = [&](const fs::path& file, const std::vector<std::string>& args) {
        const auto result = runner(file, args, process);
        if (result.exit_code || result.stdout_capture_truncated || result.stderr_capture_truncated)
            throw Error("MANAGED_VERIFICATION_FAILED: " + path_text(file.filename()));
        return result.stdout_text;
    };
    Json receipt;
    if (!options.allow_local_build) {
        if (!hex_id(options.source, 40) || options.target.empty() || options.receipt.empty() ||
            options.bundle.empty())
            throw Error(
                "Signed installation requires --source SHA --target TARGET --receipt FILE --provenance FILE; "
                "--allow-local-build explicitly creates an unqualified development installation");
        const auto gh = find_program("gh");
        if (!gh && !injected)
            throw Error("GitHub CLI is required only for signed installation/promotion verification; "
                        "normal native startup and supervision do not need it");
        // Authenticate the bytes and receipt before parsing claims or executing the candidate.
        auto verifier_options = process;
        verifier_options.env = worker_environment();
        for (const auto* key : {"GH_TOKEN", "GITHUB_TOKEN", "GH_HOST", "GH_CONFIG_DIR"})
            if (const auto value = environment(key))
                (*verifier_options.env)[key] = *value;
        const auto receipt_hash = sha256_file(options.receipt);
        for (const auto& file : {binary, fs::canonical(options.receipt)}) {
            const auto verified =
                runner(gh.value_or(fs::path("gh")),
                       {"attestation", "verify", path_text(file), "--repo", "adybag14-cyber/devbox",
                        "--signer-workflow", "adybag14-cyber/devbox/.github/workflows/cpp-runtime.yml",
                        "--source-digest", options.source, "--deny-self-hosted-runners", "--bundle",
                        path_text(fs::canonical(options.bundle)), "--format", "json"},
                       verifier_options);
            if (verified.exit_code || verified.stdout_capture_truncated || verified.stderr_capture_truncated)
                throw Error("MANAGED_ATTESTATION_VERIFICATION_FAILED");
        }
        if (sha256_file(options.receipt) != receipt_hash)
            throw Error("QUALIFICATION_RECEIPT_CHANGED");
        receipt = read_json(options.receipt, 8 * 1024 * 1024);
        if (json_uint(receipt, "schema") != 1 || json_string(receipt, "sourceSha") != options.source ||
            !hex_id(json_string(receipt, "sourceTree"), 40) ||
            json_string(receipt, "qualification") != "complete_required_workflow_dependency_graph")
            throw Error("QUALIFICATION_RECEIPT_IDENTITY_MISMATCH");
        const auto workflow = json_string(receipt, "workflowRunId");
        if (workflow.empty() ||
            !std::all_of(workflow.begin(), workflow.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw Error("QUALIFICATION_HOSTED_WORKFLOW_REQUIRED");
        const auto jobs = json_strings(receipt, "requiredJobs");
        for (const auto* name : {"native", "android", "termux", "distributions", "alpine", "security"})
            if (std::find(jobs.begin(), jobs.end(), name) == jobs.end())
                throw Error("QUALIFICATION_INCOMPLETE_REQUIRED_JOBS");
        unsigned matches = 0;
        for (const auto& target : receipt.at("targets")) {
            if (json_string(target, "target") != options.target)
                continue;
            if (json_uint(target.at("assurance"), "schema") != 1)
                throw Error("QUALIFICATION_DEPENDENCY_ASSURANCE_REQUIRED");
            for (const auto& record : target.at("binaries"))
                if (json_string(record, "name") == "devbox-mcp" && json_string(record, "sha256") == hash &&
                    json_uint(record, "bytes") == fs::file_size(binary))
                    ++matches;
        }
        if (matches != 1)
            throw Error("QUALIFICATION_BINARY_DIGEST_MISMATCH");
    }
    if (sha256_file(binary) != hash)
        throw Error("MANAGED_BINARY_CHANGED_DURING_VERIFICATION");
    const auto info = Json::parse(run(binary, {"--build-info"}));
    if (json_string(info, "implementation") != "cpp" || json_string(info, "binarySha256") != hash ||
        json_uint(info, "contractVersion") != cpp_contract_version ||
        json_uint(info, "toolCount") != cpp_tool_count ||
        json_uint(info, "stateSchemaVersion") != state_store_schema_version ||
        json_uint(info, "stateCoordinatorProtocol") != 1 || json_uint(info, "nativeManagementVersion") != 1)
        throw Error("MANAGED_BINARY_CONTRACT_OR_STATE_INCOMPATIBLE");
    if (!options.allow_local_build &&
        (info.at("sourceDirty") != false || info.at("sanitizers") != false ||
         json_string(info, "gitSha") != options.source || info.at("sourceTree") != receipt.at("sourceTree")))
        throw Error("MANAGED_BINARY_SOURCE_NOT_QUALIFIED");
    const auto tools = Json::parse(run(binary, {"--dump-contract"}));
    const auto& registry = tool_registry();
    std::set<std::string> actual, expected;
    for (const auto& tool : tools)
        actual.insert(tool.at("name").get<std::string>());
    for (const auto& tool : registry.at("tools"))
        expected.insert(tool.at("name").get<std::string>());
    if (!tools.is_array() || tools.size() != cpp_tool_count || actual != expected)
        throw Error("MANAGED_BINARY_CAPABILITIES_MISMATCH");
    if (sha256_file(binary) != hash)
        throw Error("MANAGED_BINARY_CHANGED_DURING_VERIFICATION");
    return Json{{"file", path_text(binary)},
                {"sha256", hash},
                {"build", info},
                {"policy", options.allow_local_build ? "local-development" : "signed-qualified"},
                {"source", json_string(info, "gitSha")},
                {"target", options.target},
                {"verifiedAt", utc_now()},
                {"workflowRunId", receipt.is_object() ? receipt.at("workflowRunId") : Json()}};
}
} // namespace devbox
