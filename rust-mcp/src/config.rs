use std::{
    env,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use serde::Serialize;

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

#[derive(Debug, Clone)]
pub struct Config {
    pub project_root: PathBuf,
    pub host: String,
    pub port: u16,
    pub auth_mode: AuthMode,
    pub runtime_mode: RuntimeMode,
    pub platform: Platform,
    pub public_base_url: Option<String>,
    pub host_workspace_path: PathBuf,
    pub devbox_workspace_path: PathBuf,
    pub devbox_container_name: String,
    pub devbox_image_name: String,
    pub devbox_tmp_volume_name: String,
    pub devbox_retired_container_grace_ms: u64,
    pub devbox_auto_start: bool,
    pub devbox_default_user: String,
    pub host_default_workdir: PathBuf,
    pub host_shell: String,
    pub node_exe: String,
    pub host_program_allowlist: Vec<String>,
    pub devbox_program_allowlist: Vec<String>,
    pub host_exec_enabled: bool,
    pub execution_slot_root: PathBuf,
    pub jobs_root: PathBuf,
    pub exec_max_concurrent: usize,
    pub exec_reserved_interactive: usize,
    pub exec_queue_timeout_ms: u64,
    pub background_queue_timeout_ms: u64,
    pub watch_max_concurrent: usize,
    pub exec_heavy_weight: usize,
    pub job_log_max_bytes: u64,
    pub job_log_rotations: usize,
    pub job_heartbeat_ms: u64,
    pub job_orphan_stale_ms: u64,
    pub job_retention_hours: u64,
    pub max_wait_seconds: f64,
    pub max_mcp_transfer_chars: usize,
}

struct ProgramConfiguration {
    host_shell: String,
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
        let public_base_url = env::var("PUBLIC_BASE_URL")
            .ok()
            .map(|value| value.trim().trim_end_matches('/').to_owned())
            .filter(|value| !value.is_empty());
        if auth_mode != AuthMode::None && public_base_url.is_none() {
            bail!("PUBLIC_BASE_URL is required when MCP_AUTH_MODE uses OAuth");
        }

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
        let devbox_container_name = env::var("DEVBOX_CONTAINER_NAME")
            .ok()
            .filter(|value| !value.trim().is_empty())
            .unwrap_or_else(|| "chatgpt-devbox-runtime".to_owned());
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
        let devbox_default_user = env::var("DEVBOX_DEFAULT_USER")
            .ok()
            .map(|value| value.trim().to_owned())
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| {
                if runtime_mode == RuntimeMode::Docker {
                    "root".to_owned()
                } else {
                    env::var("USERNAME")
                        .or_else(|_| env::var("USER"))
                        .unwrap_or_default()
                }
            });
        let program = load_program_configuration(&platform, runtime_mode);
        let execution_slot_root = env_path("MCP_EXEC_SLOT_ROOT")
            .unwrap_or_else(|| project_root.join("run").join("execution-slots"));
        let jobs_root =
            env_path("MCP_JOBS_ROOT").unwrap_or_else(|| project_root.join("run").join("jobs"));

        Ok(Self {
            project_root,
            host: env::var("HOST").unwrap_or_else(|_| "0.0.0.0".to_owned()),
            port: parse_env("PORT", 8100_u16),
            auth_mode,
            runtime_mode,
            platform,
            public_base_url,
            host_workspace_path,
            devbox_workspace_path,
            devbox_container_name,
            devbox_image_name,
            devbox_tmp_volume_name,
            devbox_retired_container_grace_ms: parse_env(
                "DEVBOX_RETIRED_CONTAINER_GRACE_MS",
                300_000_u64,
            ),
            devbox_auto_start: parse_bool_env("DEVBOX_AUTO_START", true),
            devbox_default_user,
            host_default_workdir,
            host_shell: program.host_shell,
            node_exe: program.node_exe,
            host_program_allowlist: program.host_program_allowlist,
            devbox_program_allowlist: program.devbox_program_allowlist,
            host_exec_enabled: parse_bool_env("ENABLE_HOST_EXEC", true),
            execution_slot_root,
            jobs_root,
            exec_max_concurrent: parse_env("MCP_EXEC_MAX_CONCURRENT", 6_usize),
            exec_reserved_interactive: parse_env("MCP_EXEC_RESERVED_INTERACTIVE", 1_usize),
            exec_queue_timeout_ms: parse_env("MCP_EXEC_QUEUE_TIMEOUT_MS", 15_000_u64),
            background_queue_timeout_ms: parse_env("MCP_BACKGROUND_QUEUE_TIMEOUT_MS", 300_000_u64),
            watch_max_concurrent: parse_env("MCP_WATCH_MAX_CONCURRENT", 4_usize),
            exec_heavy_weight: parse_env("MCP_EXEC_HEAVY_WEIGHT", 2_usize),
            job_log_max_bytes: parse_env("MCP_JOB_LOG_MAX_BYTES", 32_u64 * 1024 * 1024),
            job_log_rotations: parse_env("MCP_JOB_LOG_ROTATIONS", 2_usize),
            job_heartbeat_ms: parse_env("MCP_JOB_HEARTBEAT_MS", 5_000_u64),
            job_orphan_stale_ms: parse_env("MCP_JOB_ORPHAN_STALE_MS", 15_000_u64),
            job_retention_hours: parse_env("MCP_JOB_RETENTION_HOURS", 168_u64),
            max_wait_seconds: f64::from(parse_env("MCP_WAIT_MAX_SECONDS", 300_u16)),
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
}

fn load_env_layers(project_root: &Path) {
    // Match the JS launcher's effective precedence: .env.runtime is loaded by
    // the process launcher first, then .env may only fill missing values.
    let _ = dotenvy::from_path(project_root.join(".env.runtime"));
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

fn load_program_configuration(
    platform: &Platform,
    runtime_mode: RuntimeMode,
) -> ProgramConfiguration {
    let host_shell = env::var("HOST_SHELL")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| default_host_shell(platform));
    let host_program_allowlist = parse_csv_env(
        "HOST_PROGRAM_ALLOWLIST",
        default_host_program_allowlist(platform),
    );
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

fn usable_executable_candidate(candidate: &str) -> bool {
    let candidate = candidate.trim();
    !candidate.is_empty() && (!Path::new(candidate).is_absolute() || Path::new(candidate).is_file())
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

fn parse_transfer_limit() -> usize {
    let raw = env::var("MAX_MCP_TRANSFER_CHARS").unwrap_or_default();
    let normalized = raw.trim().to_ascii_lowercase();
    if matches!(
        normalized.as_str(),
        "0" | "-1" | "none" | "off" | "disabled" | "unlimited" | "infinite" | "infinity"
    ) {
        return usize::MAX;
    }
    normalized
        .parse::<usize>()
        .ok()
        .filter(|value| *value > 0)
        .unwrap_or(4_000_000)
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
mod tests {
    use super::*;

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
