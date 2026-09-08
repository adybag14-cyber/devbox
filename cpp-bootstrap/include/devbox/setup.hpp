#pragma once
#include "devbox/config.hpp"
#include "devbox/process.hpp"
namespace devbox::setup {
inline constexpr std::string_view installer_version = DEVBOX_SETUP_VERSION;
enum class PlatformKind { windows, macos, linux, termux, other };
struct Options {
    std::optional<fs::path> repo, workspace, runtime_binary;
    std::string repo_url = "https://github.com/adybag14-cyber/devbox.git";
    std::optional<std::string> runtime, host, auth, public_url, team_domain, audience, jwks_url;
    std::optional<std::uint16_t> port;
    bool system_packages = true, dependencies = true, link = true, start = true, guardian = false;
    bool dry_run = false, help = false, version = false, build_only = false;
};
struct PreparedConfig {
    std::string host, runtime, content;
    std::uint16_t port = 8100;
    Environment environment;
};
Options parse_options(const std::vector<std::string>& args);
std::string usage();
std::optional<std::string> env_key(std::string_view line);
std::optional<std::string> get_env_value(std::string_view content, std::string_view key);
std::string set_env_value(std::string content, std::string_view key, std::string_view value);
Environment collect_env_values(std::string_view content);
bool is_termux(std::optional<std::string> version, std::optional<std::string> prefix);
PlatformKind platform_kind();
std::string platform_name(PlatformKind platform);
std::string recommended_runtime(PlatformKind platform, bool docker_available);
std::optional<unsigned> node_major(std::string version);
bool is_repo(const fs::path& root);
PreparedConfig prepare_files(const fs::path& root, const Options& options);
std::string loopback_host(std::string host);
std::string host_port(std::string host, unsigned port);
bool health_check(const std::string& host, unsigned port);
std::string cloudflare_hint(PlatformKind platform, std::string manager = {});
fs::path build_runtime(const fs::path& root, const Options& options);
void run(const Options& options);
} // namespace devbox::setup
