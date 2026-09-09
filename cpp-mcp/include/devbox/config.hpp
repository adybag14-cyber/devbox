#pragma once
#include "common.hpp"

namespace devbox {
enum class AuthMode { none, demo_oauth, cloudflare_access };
enum class RuntimeMode { host, docker };
struct Platform {
    std::string id, display_name;
    bool is_windows = false, is_macos = false, is_linux = false, is_android = false, is_termux = false;
    static Platform detect();
};
struct Config {
    fs::path project_root;
    std::string host;
    std::uint16_t port = 8100;
    AuthMode auth_mode = AuthMode::none;
    RuntimeMode runtime_mode = RuntimeMode::host;
    Platform platform;
    std::optional<std::string> public_base_url;
    bool gateway_bridge_enabled = false;
    std::vector<std::string> gateway_bridge_origins;
    fs::path oauth_state_file_path;
    std::optional<std::string> cloudflare_access_team_domain, cloudflare_access_jwks_url;
    std::string cloudflare_access_aud;
    fs::path host_workspace_path, devbox_workspace_path, host_default_workdir;
    std::string devbox_container_name, devbox_image_name, devbox_tmp_volume_name, devbox_default_user;
    std::uint64_t devbox_retired_container_grace_ms = 300000, devbox_version_cache_ms = 120000,
                  docker_command_timeout_ms = 120000;
    bool devbox_auto_start = true;
    std::string host_shell, power_shell_exe, power_shell_fallback_exe, node_exe;
    std::vector<std::string> host_program_allowlist, devbox_program_allowlist;
    std::string host_search_backend;
    bool host_exec_enabled = false, allow_windows_host_exec_uac = false;
    fs::path execution_slot_root, jobs_root, mcp_performance_state_path;
    std::uint64_t usage_log_max_bytes = 16 * 1024 * 1024;
    std::size_t usage_log_rotations = 3, mcp_json_body_limit_bytes = 8 * 1024 * 1024, oauth_max_clients = 256;
    std::size_t exec_max_concurrent = 6, exec_reserved_interactive = 1, watch_max_concurrent = 4;
    std::uint64_t exec_queue_timeout_ms = 15000, background_queue_timeout_ms = 300000;
    std::size_t exec_heavy_capacity = 4, exec_heavy_weight = 2, exec_io_heavy_capacity = 2,
                exec_io_heavy_weight = 2;
    std::uint64_t background_priority_age_ms = 30000, job_log_max_bytes = 32 * 1024 * 1024;
    std::size_t job_log_rotations = 2;
    std::uint64_t job_heartbeat_ms = 5000, job_orphan_stale_ms = 15000, job_retention_hours = 168;
    std::uint64_t job_store_max_bytes = 2ULL * 1024 * 1024 * 1024;
    std::size_t job_store_max_terminal_jobs = 5000, job_max_active = 16, job_max_per_task = 8,
                job_max_operations = 10000;
    std::uint64_t screen_capture_attempt_timeout_ms = 10000, screen_capture_queue_timeout_ms = 15000;
    std::size_t screen_capture_retries = 1;
    double max_wait_seconds = 300;
    std::size_t command_output_limit_chars = 65536, max_mcp_transfer_chars = 4000000;
    static Config load();
    std::string server_name() const;
    std::string runtime_label() const;
    std::string auth_name() const;
    std::string runtime_name() const;
    std::string public_url() const;
};
std::string normalize_program(std::string value);
Json parse_env_text(std::string_view text);
void load_env_layers(const fs::path& root);
fs::path discover_project_root();
} // namespace devbox
