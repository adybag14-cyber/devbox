use std::{
    env,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use serde::Serialize;
use url::Url;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum AuthMode {
    None,
    DemoOauth,
    CloudflareAccess,
}

impl AuthMode {
    fn parse(value: &str) -> Result<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "none" | "" => Ok(Self::None),
            "demo-oauth" => Ok(Self::DemoOauth),
            "cloudflare-access" => Ok(Self::CloudflareAccess),
            other => bail!("unsupported MCP_AUTH_MODE {other:?}"),
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::DemoOauth => "demo-oauth",
            Self::CloudflareAccess => "cloudflare-access",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum RuntimeMode {
    Host,
    Docker,
}

impl RuntimeMode {
    fn resolve(requested: Option<&str>, platform: &Platform) -> Result<Self> {
        match requested
            .unwrap_or_default()
            .trim()
            .to_ascii_lowercase()
            .as_str()
        {
            "docker" => Ok(Self::Docker),
            "" | "auto" if platform.is_windows => Ok(Self::Docker),
            "host" | "" | "auto" => Ok(Self::Host),
            other => bail!("unsupported DEVBOX_RUNTIME_MODE {other:?}"),
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Host => "host",
            Self::Docker => "docker",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[allow(
    clippy::struct_excessive_bools,
    reason = "mirrors the JavaScript platform capability contract"
)]
pub struct Platform {
    pub id: String,
    pub display_name: String,
    pub is_windows: bool,
    pub is_macos: bool,
    pub is_linux: bool,
    pub is_android: bool,
    pub is_termux: bool,
}

impl Platform {
    #[must_use]
    pub fn detect() -> Self {
        let prefix = env::var("PREFIX").unwrap_or_default();
        let is_windows = cfg!(target_os = "windows");
        let is_macos = cfg!(target_os = "macos");
        let is_android = cfg!(target_os = "android");
        let is_linux = cfg!(target_os = "linux");
        let is_termux = (is_linux || is_android)
            && (env::var_os("TERMUX_VERSION").is_some() || prefix.contains("com.termux/files/usr"));
        let (id, display_name) = if is_termux {
            ("termux", "Termux")
        } else if is_windows {
            ("windows", "Windows")
        } else if is_macos {
            ("macos", "macOS")
        } else if is_linux {
            ("linux", "Linux")
        } else if is_android {
            ("android", "Android")
        } else {
            (env::consts::OS, env::consts::OS)
        };
        Self {
            id: id.to_owned(),
            display_name: display_name.to_owned(),
            is_windows,
            is_macos,
            is_linux,
            is_android,
            is_termux,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HostSearchBackend {
    Auto,
    Ripgrep,
    Rust,
}

impl HostSearchBackend {
    fn parse(value: &str) -> Result<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "" | "auto" => Ok(Self::Auto),
            "rg" => Ok(Self::Ripgrep),
            "js" => Ok(Self::Rust),
            other => bail!(
                "Unsupported HOST_SEARCH_BACKEND \"{other}\". Use \"auto\", \"rg\", or \"js\"."
            ),
        }
    }
}

#[derive(Debug, Clone)]
pub struct GatewayBridgeConfig {
    pub enabled: bool,
    pub origins: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct UsageLogConfig {
    pub max_bytes: u64,
    pub rotations: usize,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub project_root: PathBuf,
    pub host: String,
    pub port: u16,
    pub auth_mode: AuthMode,
    pub runtime_mode: RuntimeMode,
    pub platform: Platform,
    pub public_base_url: Option<String>,
    pub gateway_bridge: GatewayBridgeConfig,
    pub oauth_state_file_path: PathBuf,
    pub cloudflare_access_team_domain: Option<String>,
    pub cloudflare_access_aud: String,
    pub cloudflare_access_jwks_url: Option<String>,
    pub host_workspace_path: PathBuf,
    pub devbox_workspace_path: PathBuf,
    pub devbox_container_name: String,
    pub devbox_image_name: String,
    pub devbox_tmp_volume_name: String,
    pub devbox_retired_container_grace_ms: u64,
    pub devbox_auto_start: bool,
    pub devbox_version_cache_ms: u64,
    pub docker_command_timeout_ms: u64,
    pub devbox_default_user: String,
    pub host_default_workdir: PathBuf,
    pub host_shell: String,
    pub power_shell_exe: String,
    pub power_shell_fallback_exe: String,
    pub node_exe: String,
    pub host_program_allowlist: Vec<String>,
    pub devbox_program_allowlist: Vec<String>,
    pub host_search_backend: HostSearchBackend,
    pub host_exec_enabled: bool,
    pub allow_windows_host_exec_uac: bool,
    pub execution_slot_root: PathBuf,
    pub jobs_root: PathBuf,
    pub mcp_performance_state_path: PathBuf,
    pub usage_log: UsageLogConfig,
    pub mcp_json_body_limit_bytes: usize,
    pub exec_max_concurrent: usize,
    pub exec_reserved_interactive: usize,
    pub exec_queue_timeout_ms: u64,
    pub background_queue_timeout_ms: u64,
    pub watch_max_concurrent: usize,
    pub exec_heavy_capacity: usize,
    pub exec_heavy_weight: usize,
    pub job_log_max_bytes: u64,
    pub job_log_rotations: usize,
    pub job_heartbeat_ms: u64,
    pub job_orphan_stale_ms: u64,
    pub job_retention_hours: u64,
    pub job_store_max_bytes: u64,
    pub job_store_max_terminal_jobs: usize,
    pub screen_capture_attempt_timeout_ms: u64,
    pub screen_capture_retries: usize,
    pub screen_capture_queue_timeout_ms: u64,
    pub max_wait_seconds: f64,
    pub command_output_limit_chars: usize,
    pub max_mcp_transfer_chars: usize,
}

struct OauthConfiguration {
    state_file_path: PathBuf,
    cloudflare_team_domain: Option<String>,
    cloudflare_aud: String,
    cloudflare_jwks_url: Option<String>,
}

struct ScreenCaptureConfiguration {
    attempt_timeout_ms: u64,
    retries: usize,
    queue_timeout_ms: u64,
}

struct ProgramConfiguration {
    host_shell: String,
    power_shell_exe: String,
    power_shell_fallback_exe: String,
    node_exe: String,
    host_program_allowlist: Vec<String>,
    devbox_program_allowlist: Vec<String>,
}

impl Config {
    /// Load the Rust MCP configuration using the same environment layers as the JS server.
    ///
    /// # Errors
    /// Returns an error when the project root cannot be found or runtime/auth values are invalid.
    pub fn load() -> Result<Self> {
        let project_root = discover_project_root()?;
        load_env_layers(&project_root);

        let platform = Platform::detect();
        let runtime_mode =
            RuntimeMode::resolve(env::var("DEVBOX_RUNTIME_MODE").ok().as_deref(), &platform)?;
        let auth_mode =
            AuthMode::parse(&env::var("MCP_AUTH_MODE").unwrap_or_else(|_| "none".to_owned()))?;
        let public_base_url = load_public_base_url(auth_mode)?;
        let oauth = load_oauth_configuration(&project_root, auth_mode)?;

        let host_workspace_path =
            env_path("HOST_WORKSPACE_PATH").unwrap_or_else(|| project_root.join("workspace"));
        let devbox_workspace_path = env_path("DEVBOX_WORKSPACE_PATH").unwrap_or_else(|| {
            if runtime_mode == RuntimeMode::Host {
                host_workspace_path.clone()
            } else {
                PathBuf::from("/workspace")
            }
        });
        let host_default_workdir =
            env_path("HOST_DEFAULT_WORKDIR").unwrap_or_else(|| host_workspace_path.clone());
        let devbox_container_name =
            nonempty_env_or("DEVBOX_CONTAINER_NAME", "chatgpt-devbox-runtime");
        let devbox_image_name = env::var("DEVBOX_IMAGE_NAME")
            .ok()
            .map(|value| value.trim().to_owned())
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| "chatgpt-devbox-runtime:local".to_owned());
        let devbox_tmp_volume_name = env::var("DEVBOX_TMP_VOLUME_NAME")
            .ok()
            .map(|value| value.trim().to_owned())
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| format!("{devbox_container_name}-tmp"));
        let devbox_default_user = load_devbox_default_user(runtime_mode);
        let program = load_program_configuration(&platform, runtime_mode);
        let execution_slot_root = env_path("MCP_EXEC_SLOT_ROOT")
            .unwrap_or_else(|| project_root.join("run").join("execution-slots"));
        let jobs_root =
            env_path("MCP_JOBS_ROOT").unwrap_or_else(|| project_root.join("run").join("jobs"));
        let screen_capture = load_screen_capture_configuration();
        Ok(Self {
            project_root: project_root.clone(),
            host: env::var("HOST").unwrap_or_else(|_| "0.0.0.0".to_owned()),
            port: parse_env("PORT", 8100_u16),
            auth_mode,
            runtime_mode,
            platform,
            public_base_url,
            gateway_bridge: load_gateway_bridge_configuration(),
            oauth_state_file_path: oauth.state_file_path,
            cloudflare_access_team_domain: oauth.cloudflare_team_domain,
            cloudflare_access_aud: oauth.cloudflare_aud,
            cloudflare_access_jwks_url: oauth.cloudflare_jwks_url,
            host_workspace_path,
            devbox_workspace_path,
            devbox_container_name,
            devbox_image_name,
            devbox_tmp_volume_name,
            devbox_retired_container_grace_ms: load_retired_container_grace_ms(),
            devbox_auto_start: parse_bool_env("DEVBOX_AUTO_START", true),
            devbox_version_cache_ms: parse_env("DEVBOX_VERSION_CACHE_MS", 120_000_u64),
            docker_command_timeout_ms: parse_env("DOCKER_COMMAND_TIMEOUT_MS", 120_000_u64),
            devbox_default_user,
            host_default_workdir,
            host_shell: program.host_shell,
            power_shell_exe: program.power_shell_exe,
            power_shell_fallback_exe: program.power_shell_fallback_exe,
            node_exe: program.node_exe,
            host_program_allowlist: program.host_program_allowlist,
            devbox_program_allowlist: program.devbox_program_allowlist,
            host_search_backend: load_host_search_backend()?,
            host_exec_enabled: parse_host_exec_enabled(),
            allow_windows_host_exec_uac: parse_bool_env("ALLOW_WINDOWS_HOST_EXEC_UAC", false),
            execution_slot_root,
            jobs_root,
            mcp_performance_state_path: load_performance_state_path(&project_root),
            usage_log: load_usage_log_configuration(),
            mcp_json_body_limit_bytes: parse_json_body_limit(),
            exec_max_concurrent: parse_env("MCP_EXEC_MAX_CONCURRENT", 6_usize),
            exec_reserved_interactive: parse_env("MCP_EXEC_RESERVED_INTERACTIVE", 1_usize),
            exec_queue_timeout_ms: parse_env("MCP_EXEC_QUEUE_TIMEOUT_MS", 15_000_u64),
            background_queue_timeout_ms: parse_env("MCP_BACKGROUND_QUEUE_TIMEOUT_MS", 300_000_u64),
            watch_max_concurrent: parse_env("MCP_WATCH_MAX_CONCURRENT", 4_usize),
            exec_heavy_capacity: parse_env("MCP_EXEC_HEAVY_CAPACITY", 4_usize),
            exec_heavy_weight: parse_env("MCP_EXEC_HEAVY_WEIGHT", 2_usize),
            job_log_max_bytes: parse_env("MCP_JOB_LOG_MAX_BYTES", 32_u64 * 1024 * 1024),
            job_log_rotations: parse_env("MCP_JOB_LOG_ROTATIONS", 2_usize),
            job_heartbeat_ms: parse_env("MCP_JOB_HEARTBEAT_MS", 5_000_u64),
            job_orphan_stale_ms: parse_env("MCP_JOB_ORPHAN_STALE_MS", 15_000_u64),
            job_retention_hours: parse_env("MCP_JOB_RETENTION_HOURS", 168_u64),
            job_store_max_bytes: parse_env("MCP_JOB_STORE_MAX_BYTES", 2_u64 * 1024 * 1024 * 1024),
            job_store_max_terminal_jobs: parse_env("MCP_JOB_STORE_MAX_TERMINAL_JOBS", 5_000_usize),
            screen_capture_attempt_timeout_ms: screen_capture.attempt_timeout_ms,
            screen_capture_retries: screen_capture.retries,
            screen_capture_queue_timeout_ms: screen_capture.queue_timeout_ms,
            max_wait_seconds: f64::from(parse_env("MCP_WAIT_MAX_SECONDS", 300_u16).max(1)),
            command_output_limit_chars: parse_command_output_limit(),
            max_mcp_transfer_chars: parse_transfer_limit(),
        })
    }

    #[must_use]
    pub fn server_name(&self) -> String {
        if self.runtime_mode == RuntimeMode::Docker {
            "Docker ChatGPT Devbox MCP".to_owned()
        } else {
            format!("{} Host Devbox MCP", self.platform.display_name)
        }
    }

    #[must_use]
    pub fn runtime_label(&self) -> String {
        if self.runtime_mode == RuntimeMode::Docker {
            "Docker devbox".to_owned()
        } else {
            format!("{} host devbox", self.platform.display_name)
        }
    }
    #[must_use]
    pub fn command_output_chars(&self, requested: Option<usize>) -> usize {
        requested.unwrap_or(self.command_output_limit_chars)
    }
}

fn load_env_layers(project_root: &Path) {
    // Launcher-managed processes mark .env.runtime as authoritative so a
    // production parent environment (for example PORT=8100) cannot leak into
    // a replacement child. Direct/ad-hoc Rust runs keep normal environment
    // precedence unless they explicitly opt into this launcher contract.
    let runtime_path = project_root.join(".env.runtime");
    if parse_bool_env("DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE", false) {
        let _ = dotenvy::from_path_override(&runtime_path);
    } else {
        let _ = dotenvy::from_path(&runtime_path);
    }
    let _ = dotenvy::from_path(project_root.join(".env"));
}

fn discover_project_root() -> Result<PathBuf> {
    if let Some(explicit) = env_path("DEVBOX_PROJECT_ROOT") {
        return explicit.canonicalize().or(Ok(explicit));
    }
    if let Ok(cwd) = env::current_dir()
        && let Some(found) = find_project_root(&cwd)
    {
        return Ok(found);
    }
    let exe = env::current_exe().context("resolve current executable")?;
    if let Some(parent) = exe.parent()
        && let Some(found) = find_project_root(parent)
    {
        return Ok(found);
    }
    bail!("could not discover Devbox project root; set DEVBOX_PROJECT_ROOT")
}

fn find_project_root(start: &Path) -> Option<PathBuf> {
    start.ancestors().find_map(|candidate| {
        let package = candidate.join("package.json");
        let js_server = candidate.join("src").join("server.js");
        (package.is_file() && js_server.is_file()).then(|| candidate.to_path_buf())
    })
}

fn load_devbox_default_user(runtime_mode: RuntimeMode) -> String {
    env::var("DEVBOX_DEFAULT_USER")
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| {
            if runtime_mode == RuntimeMode::Docker {
                "root".to_owned()
            } else {
                env::var("USER")
                    .or_else(|_| env::var("LOGNAME"))
                    .unwrap_or_default()
            }
        })
}

fn load_screen_capture_configuration() -> ScreenCaptureConfiguration {
    ScreenCaptureConfiguration {
        attempt_timeout_ms: parse_env("SCREEN_CAPTURE_ATTEMPT_TIMEOUT_MS", 8_000_u64),
        retries: parse_env("SCREEN_CAPTURE_RETRIES", 1_usize),
        queue_timeout_ms: parse_env("SCREEN_CAPTURE_QUEUE_TIMEOUT_MS", 5_000_u64),
    }
}

fn load_program_configuration(
    platform: &Platform,
    runtime_mode: RuntimeMode,
) -> ProgramConfiguration {
    let power_shell_exe = if platform.is_windows {
        default_host_shell(platform)
    } else {
        String::new()
    };
    let power_shell_fallback_exe = default_power_shell_fallback(platform);
    let host_shell = env::var("HOST_SHELL")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| {
            if platform.is_windows {
                power_shell_exe.clone()
            } else {
                default_host_shell(platform)
            }
        });
    let host_defaults = default_host_program_allowlist(platform);
    let host_configured = parse_csv_env("HOST_PROGRAM_ALLOWLIST", Vec::new());
    let host_extra = parse_csv_env("HOST_PROGRAM_ALLOWLIST_EXTRA", Vec::new());
    let host_program_allowlist = if parse_bool_env("HOST_PROGRAM_ALLOWLIST_REPLACE", false) {
        if host_configured.is_empty() {
            host_defaults
        } else {
            merge_program_allowlists(Vec::new(), host_configured)
        }
    } else {
        merge_program_allowlists(host_defaults, host_configured.into_iter().chain(host_extra))
    };
    let devbox_program_allowlist = parse_csv_env(
        "DEVBOX_PROGRAM_ALLOWLIST",
        if runtime_mode == RuntimeMode::Host {
            host_program_allowlist.clone()
        } else {
            default_docker_program_allowlist()
        },
    );
    let node_exe = env::var("NODE_EXE")
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "node".to_owned());
    ProgramConfiguration {
        host_shell,
        power_shell_exe,
        power_shell_fallback_exe,
        node_exe,
        host_program_allowlist,
        devbox_program_allowlist,
    }
}

fn default_host_shell(platform: &Platform) -> String {
    if !platform.is_windows {
        return env::var("SHELL").unwrap_or_else(|_| "/bin/sh".to_owned());
    }

    let configured = env::var("POWERSHELL_EXE").unwrap_or_default();
    let program_files = env::var("ProgramFiles").unwrap_or_else(|_| r"C:\Program Files".to_owned());
    let system_root = env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".to_owned());
    let candidates = [
        configured,
        PathBuf::from(program_files)
            .join("PowerShell")
            .join("7")
            .join("pwsh.exe")
            .to_string_lossy()
            .into_owned(),
        PathBuf::from(system_root)
            .join("System32")
            .join("WindowsPowerShell")
            .join("v1.0")
            .join("powershell.exe")
            .to_string_lossy()
            .into_owned(),
    ];
    candidates
        .into_iter()
        .find(|candidate| usable_executable_candidate(candidate))
        .unwrap_or_else(|| "powershell.exe".to_owned())
}

fn default_power_shell_fallback(platform: &Platform) -> String {
    if !platform.is_windows {
        return String::new();
    }
    let configured = env::var("POWERSHELL_FALLBACK_EXE").unwrap_or_default();
    let system_root = env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".to_owned());
    let legacy = PathBuf::from(system_root)
        .join("System32")
        .join("WindowsPowerShell")
        .join("v1.0")
        .join("powershell.exe")
        .to_string_lossy()
        .into_owned();
    [configured, legacy]
        .into_iter()
        .find(|candidate| usable_executable_candidate(candidate))
        .unwrap_or_else(|| "powershell.exe".to_owned())
}

fn usable_executable_candidate(candidate: &str) -> bool {
    let candidate = candidate.trim();
    !candidate.is_empty() && (!Path::new(candidate).is_absolute() || Path::new(candidate).is_file())
}

fn merge_program_allowlists<I>(mut base: Vec<String>, extra: I) -> Vec<String>
where
    I: IntoIterator<Item = String>,
{
    for value in extra {
        let value = value.trim().to_ascii_lowercase();
        if !value.is_empty() && !base.iter().any(|existing| existing == &value) {
            base.push(value);
        }
    }
    base
}

fn default_host_program_allowlist(platform: &Platform) -> Vec<String> {
    let values: &[&str] = if platform.is_windows {
        &[
            "powershell",
            "pwsh",
            "cmd",
            "git",
            "gh",
            "docker",
            "node",
            "npm",
            "npx",
            "python",
            "py",
            "pip",
            "rg",
            "curl",
            "winget",
        ]
    } else {
        &[
            "bash", "sh", "git", "gh", "node", "npm", "npx", "python", "python3", "pip", "pip3",
            "rg", "curl",
        ]
    };
    values.iter().map(|value| (*value).to_owned()).collect()
}

fn default_docker_program_allowlist() -> Vec<String> {
    [
        "bash", "sh", "git", "gh", "node", "npm", "npx", "python", "python3", "pip", "pip3", "rg",
        "curl",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect()
}

fn parse_csv_env(name: &str, fallback: Vec<String>) -> Vec<String> {
    let raw = env::var(name).unwrap_or_default();
    let source = if raw.trim().is_empty() {
        fallback
    } else {
        raw.split(',').map(str::to_owned).collect()
    };
    let mut values = Vec::new();
    for value in source {
        let trimmed = value.trim();
        if !trimmed.is_empty() && !values.iter().any(|existing| existing == trimmed) {
            values.push(trimmed.to_owned());
        }
    }
    values
}

fn load_performance_state_path(project_root: &Path) -> PathBuf {
    env_path("MCP_PERFORMANCE_STATE_PATH")
        .unwrap_or_else(|| project_root.join("run").join("mcp-performance.json"))
}

fn load_usage_log_configuration() -> UsageLogConfig {
    UsageLogConfig {
        max_bytes: parse_env("MCP_USAGE_LOG_MAX_BYTES", 16_u64 * 1024 * 1024),
        rotations: parse_env("MCP_USAGE_LOG_ROTATIONS", 3_usize),
    }
}

fn load_retired_container_grace_ms() -> u64 {
    parse_env("DEVBOX_RETIRED_CONTAINER_GRACE_MS", 300_000_u64)
}

fn load_host_search_backend() -> Result<HostSearchBackend> {
    HostSearchBackend::parse(&env::var("HOST_SEARCH_BACKEND").unwrap_or_else(|_| "auto".to_owned()))
}

fn load_gateway_bridge_configuration() -> GatewayBridgeConfig {
    GatewayBridgeConfig {
        enabled: parse_bool_env("ENABLE_GATEWAY_BRIDGE", true),
        origins: parse_csv_env(
            "GATEWAY_BRIDGE_ORIGINS",
            vec![
                "https://chatgpt.com".to_owned(),
                "https://chat.openai.com".to_owned(),
            ],
        ),
    }
}

fn nonempty_env_or(name: &str, fallback: &str) -> String {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| fallback.to_owned())
}

fn load_oauth_configuration(
    project_root: &Path,
    auth_mode: AuthMode,
) -> Result<OauthConfiguration> {
    let state_file_path = env_path("OAUTH_STATE_FILE_PATH")
        .unwrap_or_else(|| project_root.join("run").join("oauth-state.json"));
    let cloudflare_team_domain = normalized_url_env("CLOUDFLARE_ACCESS_TEAM_DOMAIN");
    let cloudflare_aud = env::var("CLOUDFLARE_ACCESS_AUD")
        .unwrap_or_default()
        .trim()
        .to_owned();
    let cloudflare_jwks_url = normalized_url_env("CLOUDFLARE_ACCESS_JWKS_URL");
    validate_oauth_configuration(
        auth_mode,
        cloudflare_team_domain.as_deref(),
        &cloudflare_aud,
    )?;
    Ok(OauthConfiguration {
        state_file_path,
        cloudflare_team_domain,
        cloudflare_aud,
        cloudflare_jwks_url,
    })
}

fn validate_oauth_configuration(
    auth_mode: AuthMode,
    cloudflare_team_domain: Option<&str>,
    cloudflare_aud: &str,
) -> Result<()> {
    if auth_mode == AuthMode::CloudflareAccess {
        if cloudflare_team_domain.is_none() {
            bail!("CLOUDFLARE_ACCESS_TEAM_DOMAIN is required when MCP_AUTH_MODE=cloudflare-access");
        }
        if cloudflare_aud.trim().is_empty() {
            bail!("CLOUDFLARE_ACCESS_AUD is required when MCP_AUTH_MODE=cloudflare-access");
        }
    }
    Ok(())
}

fn normalized_url_env(name: &str) -> Option<String> {
    env::var(name)
        .ok()
        .map(|value| value.trim().trim_end_matches('/').to_owned())
        .filter(|value| !value.is_empty())
        .map(|value| {
            if value.starts_with("http://") || value.starts_with("https://") {
                value
            } else {
                format!("https://{value}")
            }
        })
}

fn load_public_base_url(auth_mode: AuthMode) -> Result<Option<String>> {
    let value = env::var("PUBLIC_BASE_URL")
        .ok()
        .map(|value| value.trim().trim_end_matches('/').to_owned())
        .filter(|value| !value.is_empty());
    if auth_mode != AuthMode::None {
        let issuer = value.as_deref().ok_or_else(|| {
            anyhow::anyhow!("PUBLIC_BASE_URL is required when MCP_AUTH_MODE uses OAuth")
        })?;
        validate_issuer_url(
            issuer,
            parse_bool_env("MCP_DANGEROUSLY_ALLOW_INSECURE_ISSUER_URL", false),
        )?;
    }
    Ok(value)
}

fn validate_issuer_url(value: &str, allow_insecure: bool) -> Result<()> {
    let issuer = Url::parse(value).context("PUBLIC_BASE_URL must be a valid absolute URL")?;
    if issuer.scheme() != "https"
        && !matches!(issuer.host_str(), Some("localhost" | "127.0.0.1"))
        && !allow_insecure
    {
        bail!("Issuer URL must be HTTPS");
    }
    if issuer.fragment().is_some() {
        bail!("Issuer URL must not have a fragment: {value}");
    }
    if issuer.query().is_some() {
        bail!("Issuer URL must not have a query string: {value}");
    }
    Ok(())
}

fn env_path(name: &str) -> Option<PathBuf> {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_owned())
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
}

fn parse_env<T>(name: &str, fallback: T) -> T
where
    T: std::str::FromStr,
{
    env::var(name)
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(fallback)
}

fn parse_json_body_limit() -> usize {
    let raw = env::var("MCP_JSON_BODY_LIMIT").unwrap_or_else(|_| "16mb".to_owned());
    let normalized = raw.trim().to_ascii_lowercase();
    if matches!(
        normalized.as_str(),
        "0" | "-1" | "none" | "off" | "disabled" | "unlimited" | "infinite" | "infinity"
    ) {
        return usize::MAX;
    }
    let (number, multiplier) = if let Some(value) = normalized.strip_suffix("kb") {
        (value, 1024_usize)
    } else if let Some(value) = normalized.strip_suffix("mb") {
        (value, 1024_usize * 1024)
    } else if let Some(value) = normalized.strip_suffix("gb") {
        (value, 1024_usize * 1024 * 1024)
    } else if let Some(value) = normalized.strip_suffix('b') {
        (value, 1_usize)
    } else {
        (normalized.as_str(), 1_usize)
    };
    parse_scaled_size(number.trim(), multiplier).unwrap_or(16 * 1024 * 1024)
}

fn parse_scaled_size(value: &str, multiplier: usize) -> Option<usize> {
    let (whole, fraction) = value.split_once('.').map_or((value, ""), |parts| parts);
    let whole = whole.parse::<usize>().ok()?;
    let whole_bytes = whole.checked_mul(multiplier)?;
    if fraction.is_empty() {
        return (whole > 0).then_some(whole_bytes);
    }
    if !fraction.bytes().all(|value| value.is_ascii_digit()) || fraction.len() > 9 {
        return None;
    }
    let denominator = 10_usize.checked_pow(u32::try_from(fraction.len()).ok()?)?;
    let fraction = fraction.parse::<usize>().ok()?;
    let fraction_bytes = fraction.checked_mul(multiplier)?.checked_div(denominator)?;
    let total = whole_bytes.checked_add(fraction_bytes)?;
    (total > 0).then_some(total)
}

fn parse_command_output_limit() -> usize {
    let command_limit = env::var("MAX_COMMAND_OUTPUT_CHARS")
        .ok()
        .and_then(|value| value.trim().parse::<usize>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(65_536)
        .min(65_536);
    match parse_character_limit("MAX_TEXT_OUTPUT_CHARS", 4_000_000) {
        None => command_limit,
        Some(text_limit) => text_limit.min(command_limit),
    }
    .max(100)
}

fn parse_character_limit(name: &str, fallback: usize) -> Option<usize> {
    let raw = env::var(name).ok();
    parse_character_limit_value(raw.as_deref(), fallback)
}

fn parse_character_limit_value(raw: Option<&str>, fallback: usize) -> Option<usize> {
    let Some(raw) = raw else {
        return Some(fallback);
    };
    let normalized = raw.trim().to_ascii_lowercase();
    if normalized.is_empty() {
        return Some(fallback);
    }
    if matches!(
        normalized.as_str(),
        "0" | "-1" | "none" | "off" | "disabled" | "unlimited" | "infinite" | "infinity"
    ) {
        return None;
    }
    normalized
        .parse::<usize>()
        .ok()
        .filter(|value| *value > 0)
        .or(Some(fallback))
}

fn parse_host_exec_enabled() -> bool {
    if env::var_os("ENABLE_HOST_EXEC").is_some() {
        parse_bool_env("ENABLE_HOST_EXEC", true)
    } else {
        parse_bool_env("ENABLE_WINDOWS_HOST_EXEC", true)
    }
}

const DEFAULT_MCP_TRANSFER_CHARS: usize = 4_000_000;
const MIN_SAFE_MCP_TRANSFER_CHARS: usize = 262_144;

fn parse_transfer_limit() -> usize {
    let raw = env::var("MAX_MCP_TRANSFER_CHARS").ok();
    parse_transfer_limit_value(raw.as_deref())
}

fn parse_transfer_limit_value(raw: Option<&str>) -> usize {
    parse_character_limit_value(raw, DEFAULT_MCP_TRANSFER_CHARS)
        .unwrap_or(usize::MAX)
        .max(MIN_SAFE_MCP_TRANSFER_CHARS)
}

fn parse_bool_env(name: &str, fallback: bool) -> bool {
    match env::var(name) {
        Ok(value) if value.trim().is_empty() => fallback,
        Ok(value) => matches!(
            value.trim().to_ascii_lowercase().as_str(),
            "1" | "true" | "yes" | "on"
        ),
        Err(_) => fallback,
    }
}

#[cfg(test)]
pub(crate) fn test_config(root: &Path) -> Config {
    Config {
        project_root: root.to_path_buf(),
        host: "127.0.0.1".to_owned(),
        port: 0,
        auth_mode: AuthMode::None,
        runtime_mode: RuntimeMode::Host,
        platform: Platform::detect(),
        public_base_url: None,
        gateway_bridge: GatewayBridgeConfig {
            enabled: false,
            origins: vec![
                "https://chatgpt.com".to_owned(),
                "https://chat.openai.com".to_owned(),
            ],
        },
        oauth_state_file_path: root.join("oauth-state.json"),
        cloudflare_access_team_domain: None,
        cloudflare_access_aud: String::new(),
        cloudflare_access_jwks_url: None,
        host_workspace_path: root.to_path_buf(),
        devbox_workspace_path: root.to_path_buf(),
        devbox_container_name: "chatgpt-devbox-runtime".to_owned(),
        devbox_image_name: "chatgpt-devbox-runtime:local".to_owned(),
        devbox_tmp_volume_name: "chatgpt-devbox-runtime-tmp".to_owned(),
        devbox_retired_container_grace_ms: 300_000,
        devbox_auto_start: true,
        devbox_version_cache_ms: 120_000,
        docker_command_timeout_ms: 120_000,
        devbox_default_user: String::new(),
        host_default_workdir: root.to_path_buf(),
        host_shell: if cfg!(windows) { "cmd.exe" } else { "/bin/sh" }.to_owned(),
        power_shell_exe: if cfg!(windows) { "pwsh.exe" } else { "" }.to_owned(),
        power_shell_fallback_exe: if cfg!(windows) { "powershell.exe" } else { "" }.to_owned(),
        node_exe: "node".to_owned(),
        host_program_allowlist: Vec::new(),
        devbox_program_allowlist: Vec::new(),
        host_search_backend: HostSearchBackend::Auto,
        host_exec_enabled: true,
        allow_windows_host_exec_uac: false,
        execution_slot_root: root.join("execution-slots"),
        jobs_root: root.join("jobs"),
        mcp_performance_state_path: root.join("mcp-performance.json"),
        usage_log: UsageLogConfig {
            max_bytes: 16 * 1024 * 1024,
            rotations: 3,
        },
        mcp_json_body_limit_bytes: 16 * 1024 * 1024,
        exec_max_concurrent: 6,
        exec_reserved_interactive: 1,
        exec_queue_timeout_ms: 15_000,
        background_queue_timeout_ms: 300_000,
        watch_max_concurrent: 4,
        exec_heavy_capacity: 4,
        exec_heavy_weight: 2,
        job_log_max_bytes: 32 * 1024 * 1024,
        job_log_rotations: 2,
        job_heartbeat_ms: 5_000,
        job_orphan_stale_ms: 15_000,
        job_retention_hours: 168,
        job_store_max_bytes: 2 * 1024 * 1024 * 1024,
        job_store_max_terminal_jobs: 5_000,
        screen_capture_attempt_timeout_ms: 8_000,
        screen_capture_retries: 1,
        screen_capture_queue_timeout_ms: 5_000,
        max_wait_seconds: 300.0,
        command_output_limit_chars: 65_536,
        max_mcp_transfer_chars: 4_000_000,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_allowlist_merge_preserves_defaults_and_deduplicates_additions() {
        let merged = merge_program_allowlists(
            vec!["git".to_owned(), "rg".to_owned(), "curl".to_owned()],
            vec!["git".to_owned(), "custom".to_owned(), "CUSTOM".to_owned()],
        );
        assert_eq!(merged, vec!["git", "rg", "curl", "custom"]);
    }

    #[test]
    fn host_allowlist_replacement_normalizes_case_and_whitespace() {
        let replaced = merge_program_allowlists(
            Vec::new(),
            vec![" Git ".to_owned(), "CURL".to_owned(), "git".to_owned()],
        );
        assert_eq!(replaced, vec!["git", "curl"]);
    }

    #[test]
    fn host_search_backend_rejects_unknown_values() {
        assert_eq!(
            HostSearchBackend::parse("auto").expect("auto"),
            HostSearchBackend::Auto
        );
        assert_eq!(
            HostSearchBackend::parse("RG").expect("rg"),
            HostSearchBackend::Ripgrep
        );
        assert_eq!(
            HostSearchBackend::parse("js").expect("js"),
            HostSearchBackend::Rust
        );
        assert!(HostSearchBackend::parse("invalid").is_err());
    }

    #[test]
    fn transfer_limit_matches_javascript_alias_default_and_minimum_semantics() {
        assert_eq!(parse_transfer_limit_value(None), DEFAULT_MCP_TRANSFER_CHARS);
        assert_eq!(
            parse_transfer_limit_value(Some("")),
            DEFAULT_MCP_TRANSFER_CHARS
        );
        assert_eq!(parse_transfer_limit_value(Some("unlimited")), usize::MAX);
        assert_eq!(parse_transfer_limit_value(Some("OFF")), usize::MAX);
        assert_eq!(
            parse_transfer_limit_value(Some("1")),
            MIN_SAFE_MCP_TRANSFER_CHARS
        );
        assert_eq!(
            parse_transfer_limit_value(Some("10")),
            MIN_SAFE_MCP_TRANSFER_CHARS
        );
        assert_eq!(
            parse_transfer_limit_value(Some("262144")),
            MIN_SAFE_MCP_TRANSFER_CHARS
        );
        assert_eq!(
            parse_transfer_limit_value(Some("4000000")),
            DEFAULT_MCP_TRANSFER_CHARS
        );
        assert_eq!(
            parse_transfer_limit_value(Some("not-a-number")),
            DEFAULT_MCP_TRANSFER_CHARS
        );
    }

    #[test]
    fn json_body_scaled_sizes_match_express_style_units() {
        assert_eq!(parse_scaled_size("1", 1024), Some(1024));
        assert_eq!(parse_scaled_size("1.5", 1024), Some(1536));
        assert_eq!(parse_scaled_size("0.5", 1024), Some(512));
        assert_eq!(parse_scaled_size("bad", 1024), None);
    }

    #[test]
    fn oauth_issuer_security_matches_installed_sdk() {
        assert!(validate_issuer_url("https://example.com/mcp", false).is_ok());
        assert!(validate_issuer_url("http://localhost:8100", false).is_ok());
        assert!(validate_issuer_url("http://127.0.0.1:8100", false).is_ok());
        assert!(validate_issuer_url("http://example.com", false).is_err());
        assert!(validate_issuer_url("http://example.com", true).is_ok());
        assert!(validate_issuer_url("https://example.com/?x=1", false).is_err());
        assert!(validate_issuer_url("https://example.com/#fragment", false).is_err());
    }

    #[test]
    fn windows_auto_runtime_matches_javascript_default() {
        let platform = Platform {
            id: "windows".to_owned(),
            display_name: "Windows".to_owned(),
            is_windows: true,
            is_macos: false,
            is_linux: false,
            is_android: false,
            is_termux: false,
        };
        assert_eq!(
            RuntimeMode::resolve(Some("auto"), &platform).unwrap(),
            RuntimeMode::Docker
        );
    }
}
