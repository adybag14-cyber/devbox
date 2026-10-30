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
        {
            const Json previous{{"sha256", std::string(64, 'a')}};
            const Json candidate{{"sha256", std::string(64, 'b')}};
            const Json replacement{{"sha256", std::string(64, 'c')}};
            const Json handoff{{"previous", previous}, {"candidate", candidate}};
            validate_managed_handoff_selection(previous, handoff);
            validate_managed_handoff_selection(candidate, handoff);
            rejects([&] { validate_managed_handoff_selection(replacement, handoff); },
                    "HANDOFF_SELECTION_CHANGED");
            rejects([&] { validate_managed_handoff_selection(Json::object(), handoff); },
                    "HANDOFF_SELECTION_CHANGED");
        }
        {
            const std::string hash(64, 'a'), source(40, 'b'), pipe = "Devbox-Owned-Test";
#ifdef _WIN32
            const auto file = root / "run" / "native" / "releases" / hash / "devbox-mcp.exe";
#else
            const auto file = root / "run" / "native" / "releases" / hash / "devbox-mcp";
#endif
            const Json build{{"binarySha256", hash}, {"gitSha", source}, {"sourceDirty", false}};
            const Json selection{
                {"schema", 1},
                {"root", path_text(root)},
                {"desired", true},
                {"policy", "signed-qualified"},
                {"environment", {{"DEVBOX_COMPUTER_USE_PIPE", pipe}}},
                {"current", {{"file", path_text(file)}, {"sha256", hash}, {"source", source}}}};
            validate_managed_computer_selection(root, selection, build, hash, source, pipe);
            for (const auto* field : {"file", "sha256", "source"}) {
                auto changed = selection;
                changed["current"][field] = "different";
                rejects(
                    [&] { validate_managed_computer_selection(root, changed, build, hash, source, pipe); },
                    "SELECTION_MISMATCH");
            }
            auto dirty = build;
            dirty["sourceDirty"] = true;
            rejects([&] { validate_managed_computer_selection(root, selection, dirty, hash, source, pipe); },
                    "SELECTION_MISMATCH");
            auto stopped = selection;
            stopped["desired"] = false;
            rejects([&] { validate_managed_computer_selection(root, stopped, build, hash, source, pipe); },
                    "SELECTION_MISMATCH");
            rejects(
                [&] {
                    validate_managed_computer_selection(root, selection, build, hash, source, "different");
                },
                "SELECTION_MISMATCH");
            rejects(
                [&] { validate_managed_computer_selection(root, selection, build, hash, source, "../pipe"); },
                "ARGUMENTS_INVALID");
        }
        const std::string memory_unit = "devbox-native-fallback.service";
        constexpr std::uint64_t physical = 576 * 1024 * 1024, combined = 1024 * 1024 * 1024;
        const auto legacy = service_memory_plan("8:memory:/system.slice/" + memory_unit + "\n0::/other\n",
                                                memory_unit, physical, combined);
        require(!legacy.unified && legacy.group == "/system.slice/" + memory_unit,
                "Hybrid cgroup must select the memory controller");
        require(service_memory_plan("0::/system.slice/" + memory_unit + "\n", memory_unit, physical, combined)
                    .unified,
                "Unified cgroup selection");
        for (const auto& member : {"0::/\n", "3:memory:/system.slice/another.service\n",
                                   "0::/system.slice/devbox-native-fallback.service/child\n"})
            rejects([&] { (void)service_memory_plan(member, memory_unit, physical, combined); },
                    "OWN_SYSTEM_UNIT");
        rejects(
            [&] {
                (void)service_memory_plan("8:memory:/system.slice/" + memory_unit +
                                              "\n9:memory:/system.slice/" + memory_unit + "\n",
                                          memory_unit, physical, combined);
            },
            "DUPLICATE");
        for (const auto* unit :
             {"ssh.service", "devbox-../other.service", "devbox-test.service/path", "devbox-test\n.service"})
            rejects([&] { (void)service_memory_plan("0::/system.slice/x\n", unit, physical, combined); },
                    "UNIT_INVALID");
        rejects(
            [&] {
                (void)service_memory_plan("0::/system.slice/" + memory_unit, memory_unit, physical,
                                          physical - 1);
            },
            "BOUNDS_INVALID");
        rejects([] { (void)parse_management_options({"start", "--root", "relative"}); }, "absolute");
        rejects(
            [&] { (void)parse_management_options({"start", "--root", path_text(root), "--unknown", "x"}); },
            "Unknown");
        rejects(
            [&] { (void)parse_management_options({"init", "--root", path_text(root), "--port", "65536"}); },
            "Invalid");
        rejects([&] { (void)management_main({"prepare", "--root", path_text(root)}); }, "absolute --output");
        rejects(
            [&] { (void)management_main({"prepare", "--root", path_text(root), "--output", "relative"}); },
            "absolute --output");
        for (const auto& output : {root, root / "inside", root.parent_path(), root / "inside" / ".."})
            rejects(
                [&] {
                    (void)management_main(
                        {"prepare", "--root", path_text(root), "--output", path_text(output)});
                },
                "OVERLAPPING_ROOTS");
        rejects(
            [&] {
                (void)management_main(
                    {"prepare", "--root", path_text(root / ""), "--output", path_text(root / "inside")});
            },
            "OVERLAPPING_ROOTS");
        const auto existing_root = root / "existing", empty_file = root / "empty-file";
        ensure_private_state_directory(existing_root);
        write_file(empty_file, "");
        rejects(
            [&] {
                (void)management_main(
                    {"prepare", "--root", path_text(existing_root), "--output", path_text(empty_file)});
            },
            "new empty output directory");
        require(fs::is_regular_file(empty_file) && fs::file_size(empty_file) == 0,
                "preparation must preserve an unrelated empty file");
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
        info["stateSchemaVersion"] = state_store_schema_version;
        BundledQualification bundled{options.source,
                                     std::string(40, 'b'),
                                     options.target,
                                     hash,
                                     "12345",
                                     fs::file_size(options.binary),
                                     {}};
        options.bundled = &bundled;
        unsigned bundled_executions = 0;
        const auto offline_runner = [&](const fs::path&, const std::vector<std::string>& args,
                                        const ProcessOptions&) {
            require(args.front() == "--build-info" || args.front() == "--dump-contract",
                    "offline qualification cannot invoke a downloader or external verifier");
            ++bundled_executions;
            ProcessOutput output;
            output.stdout_text = (args.front() == "--build-info" ? info : tools).dump();
            return output;
        };
        require(qualify_managed_binary(options, env, offline_runner).at("policy") == "bundled-qualified",
                "embedded release identity has a distinct offline policy");
        require(bundled_executions == 2, "offline native identity and contract checks");
        bundled_executions = 0;
        bundled.sha256 = std::string(64, '0');
        rejects([&] { (void)qualify_managed_binary(options, env, offline_runner); },
                "BUNDLED_QUALIFICATION_IDENTITY_MISMATCH");
        require(bundled_executions == 0, "offline digest rejection precedes candidate execution");
        bundled.sha256 = hash;
        info["sourceDirty"] = true;
        rejects([&] { (void)qualify_managed_binary(options, env, offline_runner); }, "SOURCE_NOT_QUALIFIED");
        info["sourceDirty"] = false;
        rejects([&] { (void)management_main({"promote", "--root", path_text(root)}, &bundled); },
                "INIT_ONLY");
        rejects(
            [&] {
                (void)parse_management_options(
                    {"init", "--root", path_text(root), "--bundled", "caller-supplied-proof"});
            },
            "Unknown");
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
