#include "devbox/bundled_tools.hpp"
#include "devbox/runtime.hpp"
#include "devbox/state_store.hpp"
#include <fstream>
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <typename F> void rejects(F&& fn, std::string_view reason) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(reason) != std::string_view::npos)
            return;
        throw;
    }
    throw Error("Expected refusal: " + std::string(reason));
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--host-fixture") {
        std::cout << "host-rg-selected\n";
        return 0;
    }
    try {
        const auto root = fs::temp_directory_path() / ("devbox-bundled-tools-" + uuid());
        ensure_private_state_directory(root);
        ScopeExit cleanup([&] {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        });
        ensure_private_state_directory(root / "bin");
        ensure_private_state_directory(root / "host-tools");
        const auto identity = bundled_ripgrep_identity();
        const auto file = root / "bin" / json_string(identity, "file");
        auto env = worker_environment();
        for (auto it = env.begin(); it != env.end();) {
            if (lower(it->first) == "path")
                it = env.erase(it);
            else
                ++it;
        }
        env["PATH"] = "";
        require(!host_ripgrep(env), "Empty PATH searched the current directory");
        env["PATH"] = ".";
        require(!host_ripgrep(env), "Relative PATH searched the current directory");
        auto config = std::make_shared<Config>();
        config->platform = Platform::detect();
        config->project_root = root;
        config->host_default_workdir = root;
        config->runtime_mode = RuntimeMode::host;
        config->host_exec_enabled = true;
        config->devbox_program_allowlist = {"rg"};
        config->host_program_allowlist = {"rg"};
        RuntimeExecutor runtime(config);
        ProgramRequest request;
        request.program = "rg";
        request.environment = env;
        request.working_dir = root;
        request.timeout = Millis(5000);
        request.max_capture_chars = 2048;
        const auto host = root / "host-tools" /
#ifdef _WIN32
                          "rg.exe";
#else
                          "rg";
#endif
        fs::copy_file(executable_path(), host);
        (*request.environment)["PATH"] = path_text(host.parent_path());
        request.args = {"--host-fixture"};
        require(trim(runtime.run_program(request).stdout_text) == "host-rg-selected",
                "Host ripgrep not preferred");
        (*request.environment)["PATH"] = "";
        if (argc == 2) {
            require(json_bool(identity, "compiledIdentity"), "Bundle build identity missing");
            fs::copy_file(path_from_utf8(argv[1]), file);
            auto verified = verified_bundled_ripgrep(root / "bin");
            require(verified && sha256_file(file) == json_string(identity, "sha256"),
                    "Pinned fallback did not verify");
            verified.reset();
            write_file(root / "source.txt", "first\nneedle £799\nlast\n");
            request.args = {"--no-config", "--fixed-strings", "--line-number", "needle", "source.txt"};
            require(trim(runtime.run_program(request).stdout_text) == "2:needle £799",
                    "Bundled fallback actual Unicode search failed");
            request.args = {"--version"};
            const auto version = runtime.run_host_program_only(request).stdout_text;
            require(version.find("ripgrep 15.2.0") != std::string::npos &&
                        version.find("features:-pcre2") != std::string::npos,
                    "Unexpected fallback build/features");
            {
                // Reproduce the agent-visible inventory mismatch: execution
                // worked through the bundle while status said rg=unavailable.
                const auto old_path = environment("PATH");
                set_environment("PATH", std::string());
                ScopeExit restore_path([&] { set_environment("PATH", old_path); });
                config->host_exec_enabled = false; // Inventory remains read-only metadata.
                ScopeExit restore_execution([&] { config->host_exec_enabled = true; });
                const auto versions = runtime.get_versions(true);
                require(std::any_of(versions.begin(), versions.end(),
                                    [](const auto& value) { return value.starts_with("rg=ripgrep 15.2.0"); }),
                        "Agent inventory did not discover the verified bundled fallback");
            }
            config->devbox_program_allowlist.clear();
            rejects([&] { (void)runtime.run_program(request); }, "DEVBOX_PROGRAM_ALLOWLIST");
            config->devbox_program_allowlist = {"rg"};
            config->host_exec_enabled = false;
            rejects([&] { (void)runtime.run_program(request); }, "execution is disabled");
            config->host_exec_enabled = true;
            const auto cancel = std::make_shared<Cancellation>();
            cancel->cancel();
            bool interrupted = false;
            try {
                (void)runtime.run_program(request, cancel);
            } catch (const Cancelled&) {
                interrupted = true;
            }
            require(interrupted, "Bundle verification ignored cancellation");
            std::fstream changed(file, std::ios::in | std::ios::out | std::ios::binary);
            changed.seekg(-1, std::ios::end);
            char value = 0;
            changed.read(&value, 1);
            value ^= 1;
            changed.seekp(-1, std::ios::end);
            changed.write(&value, 1);
            changed.close();
            rejects([&] { (void)runtime.run_program(request); }, "BUNDLED_RG_IDENTITY_MISMATCH");
            // A valid host tool still wins; an unused damaged fallback must not
            // disable operator-managed tools or become the chosen executable.
            (*request.environment)["PATH"] = path_text(host.parent_path());
            request.args = {"--host-fixture"};
            require(trim(runtime.run_program(request).stdout_text) == "host-rg-selected",
                    "Unused fallback overrode host rg");
        } else
            require(!json_bool(identity, "compiledIdentity"),
                    "Expected bundle path for compiled optional tool test");
        std::cout << "Native optional tool resolution, PATH safety, allowlists, digest and cancellation "
                     "checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
