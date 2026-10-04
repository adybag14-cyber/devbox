#include "devbox/contract.hpp"
#include "devbox/management.hpp"
#include "devbox/native.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class Fn> void rejects(Fn&& fn, std::string_view text) {
    try {
        require(management_instance(Json{{"instance", "18446744073709551615"}}) == UINT64_MAX,
                "process birth tokens preserve all 64 bits across JSON consumers");
        require(management_instance(Json{{"instance", "18446744073709551616"}}) == 0,
                "overflow process token rejected");
        fn();
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(text) != std::string_view::npos, error.what());
        return;
    }
    throw Error("Expected rejection: " + std::string(text));
}
} // namespace
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-native-management-unit-" + uuid());
    ensure_private_state_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        rejects([] { (void)parse_management_options({"start", "--root", "relative"}); }, "absolute");
        rejects(
            [&] { (void)parse_management_options({"start", "--root", path_text(root), "--unknown", "x"}); },
            "Unknown");
        rejects(
            [&] { (void)parse_management_options({"init", "--root", path_text(root), "--port", "65536"}); },
            "Invalid");
        const Json config{{"environment",
                           {{"PORT", "18123"},
                            {"MCP_STATE_ROOT", "/unrelated-production"},
                            {"DEVBOX_PROJECT_ROOT", "/another-project"}}}};
        const auto env = managed_environment(root, config);
        require(env.at("PORT") == "18123", "explicit port");
        require(env.at("MCP_STATE_ROOT") == path_text(root / "run" / "state"), "state isolation");
        require(env.at("DEVBOX_PROJECT_ROOT") == path_text(root), "root isolation");
        require(env.at("DEVBOX_NATIVE_MANAGED") == "1", "ambient dotenv excluded");
        auto options = parse_management_options(
            {"init", "--root", path_text(root), "--binary", path_text(root / "binary"), "--receipt",
             path_text(root / "receipt"), "--provenance", path_text(root / "bundle"), "--source",
             std::string(40, 'a'), "--target", "linux-x86_64"});
        write_file(options.binary, "owned fixture bytes");
        write_file(options.bundle, "test-only verifier input");
        const auto hash = sha256_file(options.binary);
        Json info{{"implementation", "cpp"},
                  {"binarySha256", hash},
                  {"contractVersion", cpp_contract_version},
                  {"toolCount", cpp_tool_count},
                  {"stateSchemaVersion", state_store_schema_version},
                  {"stateCoordinatorProtocol", 1},
                  {"nativeManagementVersion", 1},
                  {"sourceDirty", false},
                  {"sanitizers", false},
                  {"gitSha", options.source},
                  {"sourceTree", std::string(40, 'b')}};
        Json tools = Json::array();
        for (const auto& tool : tool_registry().at("tools"))
            tools.push_back(Json{{"name", tool.at("name")}});
        Json receipt{
            {"schema", 1},
            {"sourceSha", options.source},
            {"sourceTree", std::string(40, 'b')},
            {"workflowRunId", "12345"},
            {"qualification", "complete_required_workflow_dependency_graph"},
            {"requiredJobs", {"native", "android", "termux", "distributions", "alpine", "security"}},
            {"targets", Json::array({Json{
                            {"target", "linux-x86_64"},
                            {"assurance", {{"schema", 1}}},
                            {"binaries", Json::array({Json{{"name", "devbox-mcp"},
                                                           {"sha256", hash},
                                                           {"bytes", fs::file_size(options.binary)}}})}}})}};
        write_json_atomic(options.receipt, receipt);
        unsigned proofs = 0, executions = 0;
        const auto runner = [&](const fs::path&, const std::vector<std::string>& args,
                                const ProcessOptions& process) {
            require(process.timeout == Millis(30000), "verification bounded");
            ProcessOutput result;
            if (args.front() == "attestation") {
                ++proofs;
                require(std::find(args.begin(), args.end(), "--deny-self-hosted-runners") != args.end(),
                        "runner policy");
            } else {
                ++executions;
                require(proofs == 2, "candidate executed before both attestations verified");
                result.stdout_text = (args.front() == "--build-info" ? info : tools).dump();
            }
            return result;
        };
        require(qualify_managed_binary(options, env, runner).at("policy") == "signed-qualified",
                "valid proof");
        require(proofs == 2 && executions == 2, "verification order");
        executions = 0;
        rejects(
            [&] {
                (void)qualify_managed_binary(
                    options, env, [&](const auto&, const auto& args, const auto&) -> ProcessOutput {
                        if (args.front() != "attestation")
                            ++executions;
                        throw Error("signature rejected");
                    });
            },
            "signature rejected");
        require(executions == 0, "untrusted executable must never run");
        receipt["targets"][0]["binaries"][0]["sha256"] = std::string(64, '0');
        write_json_atomic(options.receipt, receipt);
        proofs = 0;
        rejects([&] { (void)qualify_managed_binary(options, env, runner); }, "DIGEST_MISMATCH");
        require(executions == 0, "receipt mismatch must precede execution");
        receipt["targets"][0]["binaries"][0]["sha256"] = hash;
        write_json_atomic(options.receipt, receipt);
        proofs = 0;
        info["stateSchemaVersion"] = 999;
        rejects([&] { (void)qualify_managed_binary(options, env, runner); }, "STATE_INCOMPATIBLE");
        const auto unit = native_service_definition("systemd", root / "quoted % $ binary", root);
        require(unit.find("%%") != unit.npos && unit.find("$$") != unit.npos, "systemd expansions escaped");
        require(unit.find("SendSIGKILL=no") != unit.npos, "no forced stop on busy drain");
        const auto plist = native_service_definition("launchd", root / "a&b", root);
        require(plist.find("a&amp;b") != plist.npos, "launchd XML escaping");
        rejects([&] { (void)native_service_definition("systemd", root / "bad\npath", root); }, "newlines");
        std::cout << "native management proof ordering, state boundaries and service definitions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
