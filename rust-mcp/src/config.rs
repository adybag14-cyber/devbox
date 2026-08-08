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
    pub host_default_workdir: PathBuf,
    pub host_shell: String,
    pub host_exec_enabled: bool,
    pub max_wait_seconds: f64,
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
        let host_shell = env::var("HOST_SHELL")
            .ok()
            .filter(|value| !value.trim().is_empty())
            .unwrap_or_else(|| default_host_shell(&platform));

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
            host_default_workdir,
            host_shell,
            host_exec_enabled: parse_bool_env("ENABLE_HOST_EXEC", true),
            max_wait_seconds: f64::from(parse_env("MCP_WAIT_MAX_SECONDS", 300_u16)),
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

fn default_host_shell(platform: &Platform) -> String {
    if platform.is_windows {
        env::var("POWERSHELL_EXE").unwrap_or_else(|_| "powershell.exe".to_owned())
    } else {
        env::var("SHELL").unwrap_or_else(|_| "/bin/sh".to_owned())
    }
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
