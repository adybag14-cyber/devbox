#include "devbox/config.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace devbox {
namespace {
std::optional<fs::path> env_path(std::string_view name) {
    const auto v = trim(env_or(name, ""));
    return v.empty() ? std::nullopt : std::optional(path_from_utf8(v));
}
std::string nonempty(std::string_view name, std::string_view fallback) {
    const auto v = trim(env_or(name, ""));
    return v.empty() ? std::string(fallback) : v;
}
std::optional<std::string> normalized_url(std::string_view name) {
    auto value = trim(env_or(name, ""));
    while (value.ends_with('/'))
        value.pop_back();
    if (value.empty())
        return std::nullopt;
    if (!value.starts_with("http://") && !value.starts_with("https://"))
        value = "https://" + value;
    return value;
}
std::vector<std::string> csv(std::string_view name, std::vector<std::string> fallback = {}) {
    const auto raw = trim(env_or(name, ""));
    const auto source = raw.empty() ? fallback : split(raw, ',');
    std::vector<std::string> out;
    for (const auto& value : source) {
        auto item = trim(value);
        if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end())
            out.push_back(std::move(item));
    }
    return out;
}
std::vector<std::string> merge(std::vector<std::string> values, const std::vector<std::string>& extra) {
    for (const auto& raw : extra) {
        auto value = lower(trim(raw));
        if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(std::move(value));
    }
    return values;
}
std::vector<std::string> posix_programs() {
    return {"bash",   "sh",      "git", "gh",   "node", "npm", "npx",
            "python", "python3", "pip", "pip3", "rg",   "curl"};
}
bool usable(std::string_view value) {
    if (trim(value).empty())
        return false;
    const auto p = path_from_utf8(value);
    return !p.is_absolute() || fs::is_regular_file(p);
}
std::string powershell(const Platform& platform, bool fallback) {
    if (!platform.is_windows)
        return {};
    const auto legacy = path_text(path_from_utf8(env_or("SystemRoot", "C:\\Windows")) / "System32" /
                                  "WindowsPowerShell" / "v1.0" / "powershell.exe");
    std::vector<std::string> choices{env_or(fallback ? "POWERSHELL_FALLBACK_EXE" : "POWERSHELL_EXE", "")};
    if (!fallback)
        choices.push_back(path_text(path_from_utf8(env_or("ProgramFiles", "C:\\Program Files")) /
                                    "PowerShell" / "7" / "pwsh.exe"));
    choices.push_back(legacy);
    for (const auto& value : choices)
        if (usable(value))
            return value;
    return "powershell.exe";
}
bool unlimited(std::string_view text) {
    return text == "0" || text == "-1" || text == "none" || text == "off" || text == "disabled" ||
           text == "unlimited" || text == "infinite" || text == "infinity";
}
std::optional<std::size_t> character_limit(std::string_view name, std::size_t fallback) {
    const auto raw = lower(trim(env_or(name, "")));
    if (raw.empty())
        return fallback;
    if (unlimited(raw))
        return std::nullopt;
    const auto n = env_uint(name, fallback);
    return n ? n : fallback;
}
std::size_t body_limit() {
    auto value = lower(trim(env_or("MCP_JSON_BODY_LIMIT", "16mb")));
    if (unlimited(value))
        return std::numeric_limits<std::size_t>::max();
    std::uint64_t multiplier = 1;
    if (value.ends_with("kb")) {
        multiplier = 1024;
        value.resize(value.size() - 2);
    } else if (value.ends_with("mb")) {
        multiplier = 1024 * 1024;
        value.resize(value.size() - 2);
    } else if (value.ends_with("gb")) {
        multiplier = 1024 * 1024 * 1024;
        value.resize(value.size() - 2);
    } else if (value.ends_with('b'))
        value.pop_back();
    value = trim(value);
    const auto dot = value.find('.');
    const auto whole_text = std::string_view(value).substr(0, dot);
    std::uint64_t whole = 0;
    const auto parsed = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
    if (parsed.ec != std::errc{} || parsed.ptr != whole_text.data() + whole_text.size() ||
        whole > std::numeric_limits<std::size_t>::max() / multiplier)
        return 16 * 1024 * 1024;
    std::uint64_t result = whole * multiplier;
    if (dot != value.npos) {
        const auto fraction = std::string_view(value).substr(dot + 1);
        if (!fraction.empty()) {
            if (fraction.size() > 9)
                return 16 * 1024 * 1024;
            std::uint64_t f = 0;
            const auto p = std::from_chars(fraction.data(), fraction.data() + fraction.size(), f);
            if (p.ec != std::errc{} || p.ptr != fraction.data() + fraction.size())
                return 16 * 1024 * 1024;
            std::uint64_t divisor = 1;
            for (std::size_t i = 0; i < fraction.size(); ++i)
                divisor *= 10;
            if (f > std::numeric_limits<std::uint64_t>::max() / multiplier)
                return 16 * 1024 * 1024;
            const auto extra = f * multiplier / divisor;
            if (result > std::numeric_limits<std::size_t>::max() - extra)
                return 16 * 1024 * 1024;
            result += extra;
        }
    }
    return result ? static_cast<std::size_t>(result) : 16 * 1024 * 1024;
}
std::string expand_variables(std::string value, const Json& parsed) {
    std::size_t position = 0;
    while ((position = value.find('$', position)) != value.npos) {
        auto start = position + 1, end = start;
        bool braces = start < value.size() && value[start] == '{';
        if (braces) {
            ++start;
            end = value.find('}', start);
            if (end == value.npos)
                break;
        } else {
            while (end < value.size() &&
                   ((value[end] >= 'A' && value[end] <= 'Z') || (value[end] >= 'a' && value[end] <= 'z') ||
                    (value[end] >= '0' && value[end] <= '9') || value[end] == '_'))
                ++end;
            if (end == start) {
                ++position;
                continue;
            }
        }
        const auto key = value.substr(start, end - start);
        const auto replacement =
            environment(key).value_or(parsed.contains(key) ? parsed[key].get<std::string>() : "");
        const auto consumed = (braces ? end + 1 : end) - position;
        value.replace(position, consumed, replacement);
        position += replacement.size();
    }
    return value;
}
} // namespace
Platform Platform::detect() {
    Platform result;
#ifdef _WIN32
    result.is_windows = true;
    result.id = "windows";
    result.display_name = "Windows";
#elif defined(__APPLE__)
    result.is_macos = true;
    result.id = "macos";
    result.display_name = "macOS";
#elif defined(__ANDROID__)
    result.is_android = true;
    result.id = "android";
    result.display_name = "Android";
#else
    result.is_linux = true;
    result.id = "linux";
    result.display_name = "Linux";
#endif
    if ((result.is_linux || result.is_android) &&
        (environment("TERMUX_VERSION") ||
         env_or("PREFIX", "").find("com.termux/files/usr") != std::string::npos)) {
        result.is_termux = true;
        result.id = "termux";
        result.display_name = "Termux";
    }
    return result;
}
std::string normalize_program(std::string value) {
    value = lower(trim(value));
    std::replace(value.begin(), value.end(), '\\', '/');
    const auto slash = value.rfind('/');
    if (slash != value.npos)
        value = value.substr(slash + 1);
    if (value.ends_with(".exe"))
        value.resize(value.size() - 4);
    return value;
}
Json parse_env_text(std::string_view text) {
    Json values = Json::object();
    auto lines = split(text, '\n');
    for (std::size_t index = 0; index < lines.size(); ++index) {
        auto line = trim(lines[index]);
        if (index == 0 && line.starts_with("\xEF\xBB\xBF"))
            line.erase(0, 3);
        if (line.empty() || line.starts_with('#'))
            continue;
        if (line.starts_with("export "))
            line = trim(std::string_view(line).substr(7));
        const auto equal = line.find('=');
        if (equal == line.npos)
            continue;
        const auto key = trim(std::string_view(line).substr(0, equal));
        if (key.empty())
            continue;
        auto raw = trim(std::string_view(line).substr(equal + 1));
        std::string value;
        const bool quoted = !raw.empty() && (raw[0] == '\'' || raw[0] == '"');
        if (quoted) {
            const auto quote = raw[0];
            raw.erase(0, 1);
            while (raw.find(quote) == raw.npos && index + 1 < lines.size())
                raw += '\n' + lines[++index];
            bool escape = false;
            for (const auto c : raw) {
                if (quote == '"' && escape) {
                    switch (c) {
                    case 'n':
                        value += '\n';
                        break;
                    case 'r':
                        value += '\r';
                        break;
                    case 't':
                        value += '\t';
                        break;
                    default:
                        value += c;
                        break;
                    }
                    escape = false;
                    continue;
                }
                if (quote == '"' && c == '\\') {
                    escape = true;
                    continue;
                }
                if (c == quote)
                    break;
                value += c;
            }
            if (quote == '"')
                value = expand_variables(value, values);
        } else {
            const auto comment = raw.find(" #");
            value = expand_variables(trim(std::string_view(raw).substr(0, comment)), values);
        }
        values[key] = value;
    }
    return values;
}
void load_env_layers(const fs::path& root) {
    const bool authoritative = env_bool("DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE", false);
    for (const auto& [name, override_existing] :
         std::vector<std::pair<std::string, bool>>{{".env.runtime", authoritative}, {".env", false}}) {
        const auto path = root / name;
        if (!fs::is_regular_file(path))
            continue;
        const auto values = parse_env_text(read_file(path));
        for (auto it = values.begin(); it != values.end(); ++it)
            if (override_existing || !environment(it.key()))
                set_environment(it.key(), it.value().get<std::string>());
    }
}
fs::path discover_project_root() {
    if (const auto explicit_root = env_path("DEVBOX_PROJECT_ROOT")) {
        std::error_code ec;
        auto canonical = fs::canonical(*explicit_root, ec);
        return ec ? *explicit_root : canonical;
    }
    for (auto start : std::vector<fs::path>{fs::current_path(), executable_path().parent_path()}) {
        while (!start.empty()) {
            if (fs::is_regular_file(start / "package.json") &&
                fs::is_regular_file(start / "src" / "server.js"))
                return start;
            if (start == start.parent_path())
                break;
            start = start.parent_path();
        }
    }
    throw Error("could not discover Devbox project root; set DEVBOX_PROJECT_ROOT");
}
Config Config::load() {
    Config c;
    c.project_root = discover_project_root();
    load_env_layers(c.project_root);
    c.platform = Platform::detect();
    const auto runtime = lower(trim(env_or("DEVBOX_RUNTIME_MODE", "")));
    if (runtime == "docker" || ((runtime.empty() || runtime == "auto") && c.platform.is_windows))
        c.runtime_mode = RuntimeMode::docker;
    else if (runtime == "host" || runtime.empty() || runtime == "auto")
        c.runtime_mode = RuntimeMode::host;
    else
        throw Error("unsupported DEVBOX_RUNTIME_MODE \"" + runtime + "\"");
    const auto auth = lower(trim(env_or("MCP_AUTH_MODE", "none")));
    if (auth.empty() || auth == "none")
        c.auth_mode = AuthMode::none;
    else if (auth == "demo-oauth")
        c.auth_mode = AuthMode::demo_oauth;
    else if (auth == "cloudflare-access")
        c.auth_mode = AuthMode::cloudflare_access;
    else
        throw Error("unsupported MCP_AUTH_MODE \"" + auth + "\"");
    auto public_url = trim(env_or("PUBLIC_BASE_URL", ""));
    while (public_url.ends_with('/'))
        public_url.pop_back();
    if (!public_url.empty())
        c.public_base_url = public_url;
    if (c.auth_mode != AuthMode::none) {
        if (!c.public_base_url)
            throw Error("PUBLIC_BASE_URL is required when MCP_AUTH_MODE uses OAuth");
        const auto issuer = Url::parse(public_url);
        if (issuer.scheme != "https" && issuer.host != "localhost" && issuer.host != "127.0.0.1" &&
            !env_bool("MCP_DANGEROUSLY_ALLOW_INSECURE_ISSUER_URL", false))
            throw Error("Issuer URL must be HTTPS");
        if (!issuer.fragment.empty())
            throw Error("Issuer URL must not have a fragment: " + public_url);
        if (!issuer.query.empty())
            throw Error("Issuer URL must not have a query string: " + public_url);
    }
    c.host = env_or("HOST", "0.0.0.0");
    const auto port = env_uint("PORT", 8100);
    c.port = static_cast<std::uint16_t>(port <= 65535 ? port : 8100);
    c.host_workspace_path = env_path("HOST_WORKSPACE_PATH").value_or(c.project_root / "workspace");
    c.devbox_workspace_path =
        env_path("DEVBOX_WORKSPACE_PATH")
            .value_or(c.runtime_mode == RuntimeMode::host ? c.host_workspace_path
                                                          : path_from_utf8("/workspace"));
    c.host_default_workdir = env_path("HOST_DEFAULT_WORKDIR").value_or(c.host_workspace_path);
    c.devbox_container_name = nonempty("DEVBOX_CONTAINER_NAME", "chatgpt-devbox-runtime");
    c.devbox_image_name = nonempty("DEVBOX_IMAGE_NAME", "chatgpt-devbox-runtime:local");
    c.devbox_tmp_volume_name = nonempty("DEVBOX_TMP_VOLUME_NAME", c.devbox_container_name + "-tmp");
    c.devbox_default_user =
        nonempty("DEVBOX_DEFAULT_USER",
                 c.runtime_mode == RuntimeMode::docker ? "root" : env_or("USER", env_or("LOGNAME", "")));
    c.power_shell_exe = powershell(c.platform, false);
    c.power_shell_fallback_exe = powershell(c.platform, true);
    c.host_shell =
        nonempty("HOST_SHELL", c.platform.is_windows ? c.power_shell_exe : env_or("SHELL", "/bin/sh"));
    c.node_exe = nonempty("NODE_EXE", "node");
    const auto defaults =
        c.platform.is_windows
            ? std::vector<std::string>{"powershell", "pwsh",   "cmd", "git", "gh", "docker", "node",  "npm",
                                       "npx",        "python", "py",  "pip", "rg", "curl",   "winget"}
            : posix_programs();
    const auto configured = csv("HOST_PROGRAM_ALLOWLIST"), extra = csv("HOST_PROGRAM_ALLOWLIST_EXTRA");
    c.host_program_allowlist = env_bool("HOST_PROGRAM_ALLOWLIST_REPLACE", false)
                                   ? (configured.empty() ? defaults : merge({}, configured))
                                   : merge(merge(defaults, configured), extra);
    c.devbox_program_allowlist =
        csv("DEVBOX_PROGRAM_ALLOWLIST",
            c.runtime_mode == RuntimeMode::host ? c.host_program_allowlist : posix_programs());
    c.host_search_backend = lower(trim(env_or("HOST_SEARCH_BACKEND", "auto")));
    if (c.host_search_backend.empty())
        c.host_search_backend = "auto";
    if (c.host_search_backend != "auto" && c.host_search_backend != "rg" && c.host_search_backend != "js")
        throw Error("Unsupported HOST_SEARCH_BACKEND \"" + c.host_search_backend +
                    "\". Use \"auto\", \"rg\", or \"js\".");
    c.host_exec_enabled = environment("ENABLE_HOST_EXEC") ? env_bool("ENABLE_HOST_EXEC", true)
                                                          : env_bool("ENABLE_WINDOWS_HOST_EXEC", true);
    c.allow_windows_host_exec_uac = env_bool("ALLOW_WINDOWS_HOST_EXEC_UAC", false);
    c.execution_slot_root =
        env_path("MCP_EXEC_SLOT_ROOT").value_or(c.project_root / "run" / "execution-slots");
    c.jobs_root = env_path("MCP_JOBS_ROOT").value_or(c.project_root / "run" / "jobs");
    c.mcp_performance_state_path =
        env_path("MCP_PERFORMANCE_STATE_PATH").value_or(c.project_root / "run" / "mcp-performance.json");
    c.oauth_state_file_path =
        env_path("OAUTH_STATE_FILE_PATH").value_or(c.project_root / "run" / "oauth-state.json");
    c.cloudflare_access_team_domain = normalized_url("CLOUDFLARE_ACCESS_TEAM_DOMAIN");
    c.cloudflare_access_jwks_url = normalized_url("CLOUDFLARE_ACCESS_JWKS_URL");
    c.cloudflare_access_aud = trim(env_or("CLOUDFLARE_ACCESS_AUD", ""));
    if (c.auth_mode == AuthMode::cloudflare_access) {
        if (!c.cloudflare_access_team_domain)
            throw Error("CLOUDFLARE_ACCESS_TEAM_DOMAIN is required when MCP_AUTH_MODE=cloudflare-access");
        if (c.cloudflare_access_aud.empty())
            throw Error("CLOUDFLARE_ACCESS_AUD is required when MCP_AUTH_MODE=cloudflare-access");
    }
    c.gateway_bridge_enabled = env_bool("ENABLE_GATEWAY_BRIDGE", true);
    c.gateway_bridge_origins =
        csv("GATEWAY_BRIDGE_ORIGINS", {"https://chatgpt.com", "https://chat.openai.com"});
    c.devbox_auto_start = env_bool("DEVBOX_AUTO_START", true);
#define ENV_NUMBER(field, key, fallback) c.field = static_cast<decltype(c.field)>(env_uint(key, fallback))
    ENV_NUMBER(devbox_retired_container_grace_ms, "DEVBOX_RETIRED_CONTAINER_GRACE_MS", 300000);
    ENV_NUMBER(devbox_version_cache_ms, "DEVBOX_VERSION_CACHE_MS", 120000);
    ENV_NUMBER(docker_command_timeout_ms, "DOCKER_COMMAND_TIMEOUT_MS", 120000);
    ENV_NUMBER(usage_log_max_bytes, "MCP_USAGE_LOG_MAX_BYTES", 16 * 1024 * 1024);
    ENV_NUMBER(usage_log_rotations, "MCP_USAGE_LOG_ROTATIONS", 3);
    ENV_NUMBER(oauth_max_clients, "MCP_OAUTH_MAX_CLIENTS", 256);
    c.oauth_max_clients = std::max<std::size_t>(1, c.oauth_max_clients);
    ENV_NUMBER(exec_max_concurrent, "MCP_EXEC_MAX_CONCURRENT", 6);
    ENV_NUMBER(exec_reserved_interactive, "MCP_EXEC_RESERVED_INTERACTIVE", 1);
    ENV_NUMBER(exec_queue_timeout_ms, "MCP_EXEC_QUEUE_TIMEOUT_MS", 15000);
    ENV_NUMBER(background_queue_timeout_ms, "MCP_BACKGROUND_QUEUE_TIMEOUT_MS", 300000);
    ENV_NUMBER(watch_max_concurrent, "MCP_WATCH_MAX_CONCURRENT", 4);
    ENV_NUMBER(exec_heavy_capacity, "MCP_EXEC_HEAVY_CAPACITY", 4);
    ENV_NUMBER(exec_heavy_weight, "MCP_EXEC_HEAVY_WEIGHT", 2);
    ENV_NUMBER(exec_io_heavy_capacity, "MCP_EXEC_IO_HEAVY_CAPACITY", 2);
    ENV_NUMBER(exec_io_heavy_weight, "MCP_EXEC_IO_HEAVY_WEIGHT", 2);
    ENV_NUMBER(background_priority_age_ms, "MCP_BACKGROUND_PRIORITY_AGE_MS", 30000);
    ENV_NUMBER(job_log_max_bytes, "MCP_JOB_LOG_MAX_BYTES", 32 * 1024 * 1024);
    ENV_NUMBER(job_log_rotations, "MCP_JOB_LOG_ROTATIONS", 2);
    ENV_NUMBER(job_heartbeat_ms, "MCP_JOB_HEARTBEAT_MS", 5000);
    ENV_NUMBER(job_orphan_stale_ms, "MCP_JOB_ORPHAN_STALE_MS", 15000);
    ENV_NUMBER(job_retention_hours, "MCP_JOB_RETENTION_HOURS", 168);
    ENV_NUMBER(job_store_max_bytes, "MCP_JOB_STORE_MAX_BYTES", 2ULL * 1024 * 1024 * 1024);
    ENV_NUMBER(job_store_max_terminal_jobs, "MCP_JOB_STORE_MAX_TERMINAL_JOBS", 5000);
    ENV_NUMBER(job_max_active, "MCP_JOB_MAX_ACTIVE_RUNNERS", 16);
    c.job_max_active = std::clamp<std::size_t>(c.job_max_active, 1, 128);
    ENV_NUMBER(job_max_per_task, "MCP_JOB_MAX_RUNNERS_PER_TASK", 8);
    c.job_max_per_task = std::clamp<std::size_t>(c.job_max_per_task, 1, 128);
    ENV_NUMBER(job_max_operations, "MCP_JOB_MAX_OPERATION_RECEIPTS", 10000);
    c.job_max_operations = std::clamp<std::size_t>(c.job_max_operations, 1, 100000);
    ENV_NUMBER(screen_capture_attempt_timeout_ms, "SCREEN_CAPTURE_ATTEMPT_TIMEOUT_MS", 8000);
    ENV_NUMBER(screen_capture_retries, "SCREEN_CAPTURE_RETRIES", 1);
    ENV_NUMBER(screen_capture_queue_timeout_ms, "SCREEN_CAPTURE_QUEUE_TIMEOUT_MS", 5000);
#undef ENV_NUMBER
    const auto wait = env_uint("MCP_WAIT_MAX_SECONDS", 300);
    c.max_wait_seconds = static_cast<double>(std::max<std::uint64_t>(1, wait <= 65535 ? wait : 300));
    c.mcp_json_body_limit_bytes = body_limit();
    auto limit = env_uint("MAX_COMMAND_OUTPUT_CHARS", 65536);
    if (!limit)
        limit = 65536;
    limit = std::min<std::uint64_t>(limit, 65536);
    if (const auto text_limit = character_limit("MAX_TEXT_OUTPUT_CHARS", 4000000))
        limit = std::min<std::uint64_t>(limit, *text_limit);
    c.command_output_limit_chars = static_cast<std::size_t>(std::max<std::uint64_t>(100, limit));
    c.max_mcp_transfer_chars = std::max<std::size_t>(
        262144,
        character_limit("MAX_MCP_TRANSFER_CHARS", 4000000).value_or(std::numeric_limits<std::size_t>::max()));
    return c;
}
std::string Config::server_name() const {
    return runtime_mode == RuntimeMode::docker ? "Docker ChatGPT Devbox MCP"
                                               : platform.display_name + " Host Devbox MCP";
}
std::string Config::runtime_label() const {
    return runtime_mode == RuntimeMode::docker ? "Docker devbox" : platform.display_name + " host devbox";
}
std::string Config::runtime_name() const {
    return runtime_mode == RuntimeMode::docker ? "docker" : "host";
}
std::string Config::auth_name() const {
    return auth_mode == AuthMode::demo_oauth          ? "demo-oauth"
           : auth_mode == AuthMode::cloudflare_access ? "cloudflare-access"
                                                      : "none";
}
std::string Config::public_url() const {
    return public_base_url.value_or("http://127.0.0.1:" + std::to_string(port));
}
} // namespace devbox
