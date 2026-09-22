#pragma once
#include "process.hpp"
namespace devbox {
struct IsolatedProgram {
    fs::path private_root, workspace, executable;
    std::string executable_sha256;
    std::vector<std::string> arguments;
    std::optional<std::string> input;
    Millis timeout{30000};
    std::uint64_t memory_bytes = 512ULL * 1024 * 1024, cpu_ms = 30000;
    std::size_t output_chars = 4096;
    std::function<void(std::string_view)> transition_hook;
};
// The caller is an admitted grant broker, never the general trusted-operator shell adapter.
ProcessOutput run_isolated_program(const IsolatedProgram& request, const Cancel& cancel = {});
Json isolation_capabilities();
int run_linux_isolation_worker(const fs::path& request_path);
} // namespace devbox
