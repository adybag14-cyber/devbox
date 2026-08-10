use std::env;
use std::error::Error;
use std::fmt;
use std::fs;
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

const DEFAULT_REPO_URL: &str = "https://github.com/adybag14-cyber/devbox.git";
const CANONICAL_TERMUX_REPO: &str = "https://github.com/adybag14-cyber/termux-app";
const MINIMUM_NODE_MAJOR: u32 = 18;
const MINIMUM_RUST_VERSION: (u32, u32, u32) = (1, 88, 0);
const PINNED_RUST_TOOLCHAIN: &str = "1.97.1";
const TERMUX_PACKAGES: &[&str] = &[
    "nodejs",
    "git",
    "python",
    "ripgrep",
    "curl",
    "ca-certificates",
    "rust",
];
const VERSION: &str = env!("CARGO_PKG_VERSION");

#[derive(Debug)]
struct SetupError(String);

impl fmt::Display for SetupError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for SetupError {}

type SetupResult<T> = Result<T, Box<dyn Error>>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RuntimeMode {
    Auto,
    Host,
    Docker,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum AuthChoice {
    None,
    OAuth,
    Cloudflare,
}

impl AuthChoice {
    fn parse(value: &str) -> SetupResult<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "none" => Ok(Self::None),
            "oauth" => Ok(Self::OAuth),
            "cloudflare" => Ok(Self::Cloudflare),
            _ => Err(Box::new(SetupError(format!(
                "invalid auth mode {value:?}; expected none, oauth, or cloudflare"
            )))),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::OAuth => "oauth",
            Self::Cloudflare => "cloudflare",
        }
    }

    fn env_mode(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::OAuth => "demo-oauth",
            Self::Cloudflare => "cloudflare-access",
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum PlatformKind {
    Windows,
    MacOS,
    Linux,
    Termux,
    Other,
}

impl PlatformKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::Windows => "Windows",
            Self::MacOS => "macOS",
            Self::Linux => "Linux",
            Self::Termux => "Termux / Android",
            Self::Other => "Other",
        }
    }
}

impl RuntimeMode {
    fn parse(value: &str) -> SetupResult<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "auto" => Ok(Self::Auto),
            "host" => Ok(Self::Host),
            "docker" => Ok(Self::Docker),
            _ => Err(Box::new(SetupError(format!(
                "invalid runtime mode {value:?}; expected auto, host, or docker"
            )))),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Host => "host",
            Self::Docker => "docker",
        }
    }

    fn resolved(self) -> Self {
        match self {
            Self::Auto if cfg!(windows) => Self::Docker,
            Self::Auto => Self::Host,
            explicit => explicit,
        }
    }
}

#[derive(Debug)]
struct Options {
    repo: Option<PathBuf>,
    repo_url: String,
    runtime: Option<RuntimeMode>,
    bind_host: Option<String>,
    port: Option<u16>,
    workspace: Option<PathBuf>,
    auth: Option<AuthChoice>,
    public_base_url: Option<String>,
    cloudflare_team_domain: Option<String>,
    cloudflare_aud: Option<String>,
    cloudflare_jwks_url: Option<String>,
    install_system_packages: bool,
    install_dependencies: bool,
    link_command: bool,
    start_server: bool,
    install_guardian: bool,
    dry_run: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            repo: None,
            repo_url: DEFAULT_REPO_URL.to_string(),
            runtime: None,
            bind_host: None,
            port: None,
            workspace: None,
            auth: None,
            public_base_url: None,
            cloudflare_team_domain: None,
            cloudflare_aud: None,
            cloudflare_jwks_url: None,
            install_system_packages: true,
            install_dependencies: true,
            link_command: true,
            start_server: true,
            install_guardian: false,
            dry_run: false,
        }
    }
}

fn usage() -> &'static str {
    r#"Devbox MCP bootstrap installer

USAGE:
    devbox-setup [OPTIONS]

DEFAULT BEHAVIOUR:
    - Uses the current directory when it is already a Devbox checkout.
    - Otherwise clones Devbox into ./devbox.
    - Creates .env without overwriting an existing file.
    - On Termux, installs required Android packages with `pkg`.
    - Installs npm dependencies, links the `devbox` command, and starts it.

OPTIONS:
    --repo <PATH>          Existing checkout or destination to clone into
    --repo-url <URL>       Git repository to clone (default: official Devbox repo)
    --runtime <MODE>       auto, host, or docker
    --host <ADDRESS>       Set HOST in .env
    --port <PORT>          Set PORT in .env
    --workspace <PATH>     Set host workspace and default work directory
    --auth <MODE>          Authentication: none, oauth (connector/test), or cloudflare
    --public-base-url <URL> Public MCP base URL (required for oauth/cloudflare)
    --cloudflare-team-domain <URL> Cloudflare Access team domain
    --cloudflare-aud <AUD> Cloudflare Access application audience
    --cloudflare-jwks-url <URL> Cloudflare Access JWKS URL (derived from team domain if omitted)
    --skip-system-packages Do not install Termux packages with pkg
    --skip-install         Do not run npm install
    --no-link              Do not run npm link
    --no-start             Do not start the MCP service
    --guardian             Install Guardian reliability supervision when supported
    --dry-run              Print planned commands without changing anything
    -h, --help             Show this help
    -V, --version          Show installer version
"#
}

fn next_value(arguments: &mut impl Iterator<Item = String>, flag: &str) -> SetupResult<String> {
    arguments
        .next()
        .ok_or_else(|| Box::new(SetupError(format!("{flag} requires a value"))) as Box<dyn Error>)
}

fn parse_args(arguments: impl IntoIterator<Item = String>) -> SetupResult<Options> {
    let mut options = Options::default();
    let mut arguments = arguments.into_iter();

    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--repo" => options.repo = Some(PathBuf::from(next_value(&mut arguments, "--repo")?)),
            "--repo-url" => options.repo_url = next_value(&mut arguments, "--repo-url")?,
            "--runtime" => {
                options.runtime = Some(RuntimeMode::parse(&next_value(
                    &mut arguments,
                    "--runtime",
                )?)?)
            }
            "--host" => options.bind_host = Some(next_value(&mut arguments, "--host")?),
            "--port" => {
                let value = next_value(&mut arguments, "--port")?;
                let port = value
                    .parse::<u16>()
                    .map_err(|_| SetupError(format!("invalid port {value:?}; expected 1-65535")))?;
                if port == 0 {
                    return Err(Box::new(SetupError(
                        "port must be between 1 and 65535".to_string(),
                    )));
                }
                options.port = Some(port);
            }
            "--workspace" => {
                options.workspace = Some(PathBuf::from(next_value(&mut arguments, "--workspace")?))
            }
            "--auth" => {
                options.auth = Some(AuthChoice::parse(&next_value(&mut arguments, "--auth")?)?)
            }
            "--public-base-url" => {
                options.public_base_url = Some(next_value(&mut arguments, "--public-base-url")?)
            }
            "--cloudflare-team-domain" => {
                options.cloudflare_team_domain =
                    Some(next_value(&mut arguments, "--cloudflare-team-domain")?)
            }
            "--cloudflare-aud" => {
                options.cloudflare_aud = Some(next_value(&mut arguments, "--cloudflare-aud")?)
            }
            "--cloudflare-jwks-url" => {
                options.cloudflare_jwks_url =
                    Some(next_value(&mut arguments, "--cloudflare-jwks-url")?)
            }
            "--skip-system-packages" => options.install_system_packages = false,
            "--skip-install" => options.install_dependencies = false,
            "--no-link" => options.link_command = false,
            "--no-start" => options.start_server = false,
            "--guardian" => options.install_guardian = true,
            "--dry-run" => options.dry_run = true,
            "-h" | "--help" => {
                print!("{}", usage());
                std::process::exit(0);
            }
            "-V" | "--version" => {
                println!("devbox-setup {VERSION}");
                std::process::exit(0);
            }
            unknown => {
                return Err(Box::new(SetupError(format!(
                    "unknown option {unknown:?}\n\n{}",
                    usage()
                ))));
            }
        }
    }

    Ok(options)
}

fn is_devbox_repo(path: &Path) -> bool {
    path.join("package.json").is_file()
        && path.join(".env.example").is_file()
        && path.join("src/server.js").is_file()
        && path.join("rust-mcp/Cargo.toml").is_file()
}

fn directory_is_empty(path: &Path) -> SetupResult<bool> {
    if !path.exists() {
        return Ok(true);
    }
    if !path.is_dir() {
        return Ok(false);
    }
    Ok(fs::read_dir(path)?.next().is_none())
}

fn absolute_path(path: PathBuf) -> SetupResult<PathBuf> {
    if path.is_absolute() {
        Ok(path)
    } else {
        Ok(env::current_dir()?.join(path))
    }
}

fn display_command(program: &str, arguments: &[&str]) -> String {
    let rendered = arguments
        .iter()
        .map(|argument| {
            if argument.contains(' ') {
                format!("\"{argument}\"")
            } else {
                (*argument).to_string()
            }
        })
        .collect::<Vec<_>>()
        .join(" ");
    format!("{program} {rendered}").trim().to_string()
}

fn command_name(base: &str) -> &str {
    if cfg!(windows) && base == "npm" {
        "npm.cmd"
    } else if cfg!(windows) && base == "npx" {
        "npx.cmd"
    } else {
        base
    }
}

fn run_command_with_environment(
    program: &str,
    arguments: &[&str],
    working_dir: Option<&Path>,
    dry_run: bool,
    environment: &[(String, String)],
) -> SetupResult<()> {
    println!("> {}", display_command(program, arguments));
    if dry_run {
        return Ok(());
    }

    let mut command = Command::new(program);
    command.args(arguments);
    if let Some(directory) = working_dir {
        command.current_dir(directory);
    }
    command.envs(environment.iter().map(|(key, value)| (key, value)));
    command.stdin(Stdio::inherit());
    command.stdout(Stdio::inherit());
    command.stderr(Stdio::inherit());

    let status = command
        .status()
        .map_err(|error| SetupError(format!("failed to start {program:?}: {error}")))?;
    if !status.success() {
        return Err(Box::new(SetupError(format!(
            "command failed with {status}: {}",
            display_command(program, arguments)
        ))));
    }
    Ok(())
}

fn run_command(
    program: &str,
    arguments: &[&str],
    working_dir: Option<&Path>,
    dry_run: bool,
) -> SetupResult<()> {
    run_command_with_environment(program, arguments, working_dir, dry_run, &[])
}

fn capture_command(
    program: &str,
    arguments: &[&str],
    working_dir: Option<&Path>,
) -> SetupResult<String> {
    let mut command = Command::new(program);
    command.args(arguments);
    if let Some(directory) = working_dir {
        command.current_dir(directory);
    }
    let output = command.output().map_err(|error| {
        SetupError(format!(
            "required program {program:?} is unavailable: {error}"
        ))
    })?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        return Err(Box::new(SetupError(format!(
            "{program} exited with {}{}",
            output.status,
            if stderr.is_empty() {
                String::new()
            } else {
                format!(": {stderr}")
            }
        ))));
    }
    let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
    let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
    Ok(if stdout.is_empty() { stderr } else { stdout })
}

fn parse_node_major(version: &str) -> Option<u32> {
    version
        .trim()
        .trim_start_matches('v')
        .split('.')
        .next()?
        .parse::<u32>()
        .ok()
}

fn parse_rust_version(version: &str) -> Option<(u32, u32, u32)> {
    let numeric = version
        .split_whitespace()
        .find(|part| part.chars().next().is_some_and(|ch| ch.is_ascii_digit()))?;
    let mut pieces = numeric.split('.');
    let major = pieces.next()?.parse::<u32>().ok()?;
    let minor = pieces.next()?.parse::<u32>().ok()?;
    let patch = pieces
        .next()
        .and_then(|value| value.split('-').next())
        .and_then(|value| value.parse::<u32>().ok())
        .unwrap_or(0);
    Some((major, minor, patch))
}

fn rust_version_is_supported(version: &str) -> bool {
    parse_rust_version(version).is_some_and(|parsed| parsed >= MINIMUM_RUST_VERSION)
}

fn is_termux_values(termux_version: Option<&str>, prefix: Option<&str>) -> bool {
    termux_version.is_some_and(|value| !value.trim().is_empty())
        || prefix.is_some_and(|value| value.contains("com.termux/files/usr"))
}

fn is_termux_environment() -> bool {
    let termux_version = env::var("TERMUX_VERSION").ok();
    let prefix = env::var("PREFIX").ok();
    is_termux_values(termux_version.as_deref(), prefix.as_deref())
}

fn platform_kind() -> PlatformKind {
    if is_termux_environment() {
        PlatformKind::Termux
    } else if cfg!(windows) {
        PlatformKind::Windows
    } else if cfg!(target_os = "macos") {
        PlatformKind::MacOS
    } else if cfg!(target_os = "linux") {
        PlatformKind::Linux
    } else {
        PlatformKind::Other
    }
}

fn command_available(program: &str, arguments: &[&str]) -> bool {
    Command::new(program)
        .args(arguments)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|status| status.success())
}

fn first_available_command(candidates: &[&str], arguments: &[&str]) -> Option<String> {
    candidates
        .iter()
        .find(|candidate| Path::new(candidate).is_file() || command_available(candidate, arguments))
        .map(|candidate| (*candidate).to_string())
}

fn cloudflared_install_hint_for(platform: PlatformKind, linux_manager: Option<&str>) -> String {
    match platform {
        PlatformKind::Windows =>
            "winget install --id Cloudflare.cloudflared --exact".to_string(),
        PlatformKind::MacOS => "brew install cloudflared".to_string(),
        PlatformKind::Termux =>
            "pkg update && pkg install cloudflared termux-services".to_string(),
        PlatformKind::Linux => match linux_manager.unwrap_or_default() {
            "apt-get" => concat!(
                "sudo mkdir -p --mode=0755 /usr/share/keyrings\n",
                "curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg | sudo tee /usr/share/keyrings/cloudflare-main.gpg >/dev/null\n",
                "echo \"deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main\" | sudo tee /etc/apt/sources.list.d/cloudflared.list\n",
                "sudo apt-get update && sudo apt-get install cloudflared"
            ).to_string(),
            "dnf" | "yum" => concat!(
                "curl -fsSL https://pkg.cloudflare.com/cloudflared.repo | sudo tee /etc/yum.repos.d/cloudflared.repo\n",
                "sudo dnf install cloudflared  # use yum instead of dnf on yum-based systems"
            ).to_string(),
            "pacman" => "sudo pacman -Syu cloudflared".to_string(),
            "apk" => concat!(
                "Cloudflare does not document an apk repository for cloudflared. ",
                "Install the matching official Linux binary and put it on PATH; see docs/CLOUDFLARE_TUNNEL.md."
            ).to_string(),
            _ => concat!(
                "Install the matching official cloudflared Linux package/binary and put it on PATH; ",
                "see docs/CLOUDFLARE_TUNNEL.md."
            ).to_string(),
        },
        PlatformKind::Other =>
            "See docs/CLOUDFLARE_TUNNEL.md for a supported cloudflared installation path.".to_string(),
    }
}

fn cloudflared_install_hint() -> String {
    let platform = platform_kind();
    let linux_manager = if platform == PlatformKind::Linux {
        first_available_command(
            &["apt-get", "dnf", "yum", "pacman", "zypper", "apk"],
            &["--version"],
        )
        .or_else(|| {
            first_available_command(
                &["apt-get", "dnf", "yum", "pacman", "zypper", "apk"],
                &["--help"],
            )
        })
        .and_then(|value| {
            Path::new(&value)
                .file_name()
                .and_then(|name| name.to_str())
                .map(str::to_string)
        })
    } else {
        None
    };
    cloudflared_install_hint_for(platform, linux_manager.as_deref())
}

fn cloudflare_transport_next_step(platform: PlatformKind) -> &'static str {
    match platform {
        PlatformKind::Windows =>
            "Set CLOUDFLARED_TUNNEL_TOKEN/CLOUDFLARED_PUBLIC_HOSTNAME in .env, then restart Devbox.",
        PlatformKind::MacOS | PlatformKind::Linux | PlatformKind::Termux =>
            "Set CLOUDFLARED_TUNNEL_TOKEN/CLOUDFLARED_PUBLIC_HOSTNAME in .env, then run: sh scripts/install-cloudflare-tunnel.sh auto",
        PlatformKind::Other => "See docs/CLOUDFLARE_TUNNEL.md.",
    }
}

fn termux_package_arguments() -> Vec<&'static str> {
    let mut arguments = vec!["install", "-y"];
    arguments.extend_from_slice(TERMUX_PACKAGES);
    arguments
}

fn run_owned_command(
    program: &str,
    arguments: &[String],
    working_dir: Option<&Path>,
    dry_run: bool,
) -> SetupResult<()> {
    let refs = arguments.iter().map(String::as_str).collect::<Vec<_>>();
    run_command(program, &refs, working_dir, dry_run)
}

fn is_root_user() -> bool {
    if cfg!(windows) || is_termux_environment() {
        return false;
    }
    capture_command("id", &["-u"], None).is_ok_and(|value| value.trim() == "0")
}

fn run_system_package_command(
    program: &str,
    arguments: &[String],
    dry_run: bool,
) -> SetupResult<()> {
    if cfg!(windows) || is_termux_environment() || is_root_user() {
        return run_owned_command(program, arguments, None, dry_run);
    }
    if command_available("sudo", &["--version"]) {
        let mut sudo_args = vec![program.to_string()];
        sudo_args.extend(arguments.iter().cloned());
        return run_owned_command("sudo", &sudo_args, None, dry_run);
    }
    Err(Box::new(SetupError(format!(
        "installing system packages with {program} requires root privileges; install Node.js {MINIMUM_NODE_MAJOR}+, npm, Git, Rust, and Cargo manually or rerun with sudo"
    ))))
}

fn install_system_prerequisites(options: &Options) -> SetupResult<()> {
    let node_missing = !command_available(command_name("node"), &["--version"]);
    let npm_missing = !command_available(command_name("npm"), &["--version"]);
    let git_missing = !command_available(command_name("git"), &["--version"]);
    let rustc_version = capture_command(command_name("rustc"), &["--version"], None).ok();
    let rustc_missing = rustc_version.is_none();
    let cargo_missing = !command_available(command_name("cargo"), &["--version"]);
    let rust_too_old = rustc_version
        .as_deref()
        .is_some_and(|version| !rust_version_is_supported(version));
    let rust_needs_install = rustc_missing || cargo_missing || rust_too_old;
    if !(node_missing || npm_missing || git_missing || rust_needs_install) {
        return Ok(());
    }
    if !options.install_system_packages {
        return Ok(());
    }

    match platform_kind() {
        PlatformKind::Termux => {
            println!("Termux on Android detected.");
            println!("Canonical Termux app: {CANONICAL_TERMUX_REPO}");
            let arguments = termux_package_arguments();
            run_command("pkg", &arguments, None, options.dry_run).map_err(|error| {
                Box::new(SetupError(format!(
                    "failed to install Termux packages with pkg: {error}. Use --skip-system-packages only when Node.js {MINIMUM_NODE_MAJOR}+, npm, Git, Rust, and Cargo are already installed"
                ))) as Box<dyn Error>
            })?;
        }
        PlatformKind::Windows => {
            let winget = first_available_command(&["winget.exe", "winget"], &["--version"]).ok_or_else(|| {
                Box::new(SetupError(
                    "Node.js/npm, Git, Rust, or Cargo is missing and winget was not found. Install Node.js LTS, Git for Windows, and Rustup, then rerun devbox-setup, or use --skip-system-packages.".to_string(),
                )) as Box<dyn Error>
            })?;
            if node_missing || npm_missing {
                run_owned_command(
                    &winget,
                    &[
                        "install".into(),
                        "--exact".into(),
                        "--id".into(),
                        "OpenJS.NodeJS.LTS".into(),
                        "--accept-package-agreements".into(),
                        "--accept-source-agreements".into(),
                        "--silent".into(),
                    ],
                    None,
                    options.dry_run,
                )?;
            }
            if git_missing {
                run_owned_command(
                    &winget,
                    &[
                        "install".into(),
                        "--exact".into(),
                        "--id".into(),
                        "Git.Git".into(),
                        "--accept-package-agreements".into(),
                        "--accept-source-agreements".into(),
                        "--silent".into(),
                    ],
                    None,
                    options.dry_run,
                )?;
            }
            if rust_needs_install {
                run_owned_command(
                    &winget,
                    &[
                        "install".into(),
                        "--exact".into(),
                        "--id".into(),
                        "Rustlang.Rustup".into(),
                        "--accept-package-agreements".into(),
                        "--accept-source-agreements".into(),
                        "--silent".into(),
                    ],
                    None,
                    options.dry_run,
                )?;
            }
        }
        PlatformKind::MacOS => {
            let brew = first_available_command(&["brew", "/opt/homebrew/bin/brew", "/usr/local/bin/brew"], &["--version"]).ok_or_else(|| {
                Box::new(SetupError(
                    "Node.js/npm, Git, Rust, or Cargo is missing and Homebrew was not found. Install Homebrew (or Node.js LTS, Git, Rust, and Cargo manually), then rerun devbox-setup, or use --skip-system-packages.".to_string(),
                )) as Box<dyn Error>
            })?;
            let mut packages = Vec::<String>::new();
            if node_missing || npm_missing {
                packages.push("node".into());
            }
            if git_missing {
                packages.push("git".into());
            }
            if rust_needs_install {
                packages.push("rust".into());
            }
            let mut arguments = vec!["install".to_string()];
            arguments.extend(packages);
            run_owned_command(&brew, &arguments, None, options.dry_run)?;
        }
        PlatformKind::Linux => {
            let manager = first_available_command(&["apt-get", "dnf", "yum", "pacman", "zypper", "apk"], &["--version"])
                .or_else(|| first_available_command(&["apt-get", "dnf", "yum", "pacman", "zypper", "apk"], &["--help"]))
                .ok_or_else(|| Box::new(SetupError(
                    "Node.js/npm, Git, Rust, or Cargo is missing and no supported package manager was found (apt-get, dnf, yum, pacman, zypper, apk). Install prerequisites manually or use --skip-system-packages.".to_string(),
                )) as Box<dyn Error>)?;
            let mut packages = Vec::<String>::new();
            if node_missing || npm_missing {
                packages.push("nodejs".into());
                packages.push("npm".into());
            }
            if git_missing {
                packages.push("git".into());
            }
            if rust_needs_install {
                packages.push("curl".into());
                packages.push("ca-certificates".into());
                if manager.ends_with("apt-get") {
                    packages.push("rustc".into());
                    packages.push("cargo".into());
                } else if manager.ends_with("dnf")
                    || manager.ends_with("yum")
                    || manager.ends_with("apk")
                    || manager.ends_with("zypper")
                {
                    packages.push("rust".into());
                    packages.push("cargo".into());
                } else {
                    packages.push("rust".into());
                    packages.push("cargo".into());
                }
            }
            if manager.ends_with("apt-get") {
                run_system_package_command(&manager, &["update".into()], options.dry_run)?;
                let mut args = vec!["install".into(), "-y".into()];
                args.extend(packages);
                run_system_package_command(&manager, &args, options.dry_run)?;
            } else if manager.ends_with("dnf") || manager.ends_with("yum") {
                let mut args = vec!["install".into(), "-y".into()];
                args.extend(packages);
                run_system_package_command(&manager, &args, options.dry_run)?;
            } else if manager.ends_with("pacman") {
                let mut args = vec!["-S".into(), "--needed".into(), "--noconfirm".into()];
                args.extend(packages);
                run_system_package_command(&manager, &args, options.dry_run)?;
            } else if manager.ends_with("zypper") {
                let mut args = vec!["--non-interactive".into(), "install".into()];
                args.extend(packages);
                run_system_package_command(&manager, &args, options.dry_run)?;
            } else {
                let mut args = vec!["add".into()];
                args.extend(packages);
                run_system_package_command(&manager, &args, options.dry_run)?;
            }
        }
        PlatformKind::Other => {
            return Err(Box::new(SetupError(
                "required tools are missing and automatic package installation is not available on this platform".to_string(),
            )));
        }
    }
    Ok(())
}

fn recommended_runtime_for(platform: PlatformKind, docker_available: bool) -> RuntimeMode {
    match platform {
        PlatformKind::Windows if docker_available => RuntimeMode::Auto,
        PlatformKind::Windows
        | PlatformKind::MacOS
        | PlatformKind::Linux
        | PlatformKind::Termux => RuntimeMode::Host,
        PlatformKind::Other => RuntimeMode::Auto,
    }
}

fn recommended_runtime() -> RuntimeMode {
    let docker_available = capture_command(
        command_name("docker"),
        &["version", "--format", "{{.Server.Version}}"],
        None,
    )
    .is_ok();
    recommended_runtime_for(platform_kind(), docker_available)
}

fn ensure_supported_rust_toolchain(options: &Options) -> SetupResult<()> {
    let current = capture_command(command_name("rustc"), &["--version"], None).ok();
    let cargo_ready = command_available(command_name("cargo"), &["--version"]);
    if current.as_deref().is_some_and(rust_version_is_supported) && cargo_ready {
        return Ok(());
    }

    let found = current.unwrap_or_else(|| "not installed".to_string());
    if !options.install_system_packages {
        return Err(Box::new(SetupError(format!(
            "Rust {}.{}.{}+ and Cargo are required; found {found}. Install a supported toolchain with rustup or rerun without --skip-system-packages.",
            MINIMUM_RUST_VERSION.0, MINIMUM_RUST_VERSION.1, MINIMUM_RUST_VERSION.2
        ))));
    }

    refresh_common_tool_paths();
    let mut rustup = first_available_command(&[command_name("rustup")], &["--version"]);
    if rustup.is_none() {
        match platform_kind() {
            PlatformKind::Windows => {
                return Err(Box::new(SetupError(format!(
                    "Rust remained below {}.{}.{} after installing Rustup. Reopen the shell so %USERPROFILE%\\.cargo\\bin is on PATH, then rerun devbox-setup.",
                    MINIMUM_RUST_VERSION.0, MINIMUM_RUST_VERSION.1, MINIMUM_RUST_VERSION.2
                ))));
            }
            PlatformKind::Termux => {
                return Err(Box::new(SetupError(format!(
                    "The Termux Rust package remained below {}.{}.{}. Update Termux packages and rerun devbox-setup.",
                    MINIMUM_RUST_VERSION.0, MINIMUM_RUST_VERSION.1, MINIMUM_RUST_VERSION.2
                ))));
            }
            PlatformKind::MacOS | PlatformKind::Linux => {
                let curl = first_available_command(&["curl"], &["--version"]).ok_or_else(|| {
                    Box::new(SetupError(
                        "curl is required to install a supported Rust toolchain with rustup"
                            .to_string(),
                    )) as Box<dyn Error>
                })?;
                let installer =
                    env::temp_dir().join(format!("devbox-rustup-init-{}.sh", std::process::id()));
                let download_args = vec![
                    "--proto".to_string(),
                    "=https".to_string(),
                    "--tlsv1.2".to_string(),
                    "-sSf".to_string(),
                    "https://sh.rustup.rs".to_string(),
                    "-o".to_string(),
                    installer.to_string_lossy().to_string(),
                ];
                run_owned_command(&curl, &download_args, None, options.dry_run)?;
                let install_args = vec![
                    installer.to_string_lossy().to_string(),
                    "-y".to_string(),
                    "--profile".to_string(),
                    "minimal".to_string(),
                    "--default-toolchain".to_string(),
                    PINNED_RUST_TOOLCHAIN.to_string(),
                    "--no-modify-path".to_string(),
                ];
                let result = run_owned_command("sh", &install_args, None, options.dry_run);
                if !options.dry_run {
                    let _ = fs::remove_file(&installer);
                }
                result?;
                refresh_common_tool_paths();
                rustup = first_available_command(&[command_name("rustup")], &["--version"]);
            }
            PlatformKind::Other => {}
        }
    }

    let rustup = rustup.ok_or_else(|| {
        Box::new(SetupError(format!(
            "Rust {}.{}.{}+ is required; rustup could not be installed or found on PATH",
            MINIMUM_RUST_VERSION.0, MINIMUM_RUST_VERSION.1, MINIMUM_RUST_VERSION.2
        ))) as Box<dyn Error>
    })?;
    run_owned_command(
        &rustup,
        &[
            "toolchain".into(),
            "install".into(),
            PINNED_RUST_TOOLCHAIN.into(),
            "--profile".into(),
            "minimal".into(),
        ],
        None,
        options.dry_run,
    )?;
    run_owned_command(
        &rustup,
        &["default".into(), PINNED_RUST_TOOLCHAIN.into()],
        None,
        options.dry_run,
    )?;
    refresh_common_tool_paths();
    Ok(())
}

fn refresh_common_tool_paths() {
    let separator = if cfg!(windows) { ";" } else { ":" };
    let mut entries = env::var_os("PATH")
        .map(|value| env::split_paths(&value).collect::<Vec<_>>())
        .unwrap_or_default();
    let mut candidates = Vec::<PathBuf>::new();
    if cfg!(windows) {
        if let Ok(program_files) = env::var("ProgramFiles") {
            candidates.push(PathBuf::from(&program_files).join("nodejs"));
            candidates.push(PathBuf::from(&program_files).join("Git").join("cmd"));
        }
        candidates.push(PathBuf::from(r"C:\Program Files\nodejs"));
        candidates.push(PathBuf::from(r"C:\Program Files\Git\cmd"));
        if let Some(home) = env::var_os("USERPROFILE") {
            candidates.push(PathBuf::from(home).join(".cargo").join("bin"));
        }
    } else if cfg!(target_os = "macos") {
        candidates.push(PathBuf::from("/opt/homebrew/bin"));
        candidates.push(PathBuf::from("/usr/local/bin"));
        if let Some(home) = env::var_os("HOME") {
            candidates.push(PathBuf::from(home).join(".cargo").join("bin"));
        }
    } else if let Some(home) = env::var_os("HOME") {
        candidates.push(PathBuf::from(home).join(".cargo").join("bin"));
    }
    for candidate in candidates {
        if candidate.is_dir() && !entries.iter().any(|existing| existing == &candidate) {
            entries.push(candidate);
        }
    }
    if let Ok(joined) = env::join_paths(entries) {
        env::set_var("PATH", joined);
    } else if let Ok(existing) = env::var("PATH") {
        let _ = separator;
        env::set_var("PATH", existing);
    }
}
fn verify_toolchain() -> SetupResult<()> {
    let node = capture_command(command_name("node"), &["--version"], None)?;
    let major = parse_node_major(&node).ok_or_else(|| {
        Box::new(SetupError(format!(
            "could not parse Node.js version {node:?}"
        ))) as Box<dyn Error>
    })?;
    if major < MINIMUM_NODE_MAJOR {
        return Err(Box::new(SetupError(format!(
            "Node.js {MINIMUM_NODE_MAJOR}+ is required; found {node}"
        ))));
    }
    let npm = capture_command(command_name("npm"), &["--version"], None)?;
    let git = capture_command(command_name("git"), &["--version"], None)?;
    let rustc = capture_command(command_name("rustc"), &["--version"], None)?;
    if !rust_version_is_supported(&rustc) {
        return Err(Box::new(SetupError(format!(
            "Rust {}.{}.{}+ is required; found {rustc}",
            MINIMUM_RUST_VERSION.0, MINIMUM_RUST_VERSION.1, MINIMUM_RUST_VERSION.2
        ))));
    }
    let cargo = capture_command(command_name("cargo"), &["--version"], None)?;
    println!("Node.js: {node}");
    println!("npm: {npm}");
    println!("Git: {git}");
    println!("Rust: {rustc}");
    println!("Cargo: {cargo}");
    Ok(())
}

fn clone_or_locate_repo(options: &Options) -> SetupResult<PathBuf> {
    let current = env::current_dir()?;
    let requested = match &options.repo {
        Some(path) => absolute_path(path.clone())?,
        None if is_devbox_repo(&current) => current,
        None => current.join("devbox"),
    };

    if is_devbox_repo(&requested) {
        println!("Using Devbox checkout: {}", requested.display());
        return Ok(requested);
    }

    if !directory_is_empty(&requested)? {
        return Err(Box::new(SetupError(format!(
            "{} is not a Devbox checkout and is not empty",
            requested.display()
        ))));
    }

    let parent = requested.parent().ok_or_else(|| {
        Box::new(SetupError(format!(
            "invalid repository path: {}",
            requested.display()
        ))) as Box<dyn Error>
    })?;
    if !options.dry_run {
        fs::create_dir_all(parent)?;
    }

    let git_version = capture_command(command_name("git"), &["--version"], None)?;
    println!("Git: {git_version}");
    let destination = requested.to_string_lossy().to_string();
    run_command(
        command_name("git"),
        &["clone", "--depth", "1", &options.repo_url, &destination],
        Some(parent),
        options.dry_run,
    )?;

    if !options.dry_run && !is_devbox_repo(&requested) {
        return Err(Box::new(SetupError(format!(
            "clone completed but {} does not look like a Devbox checkout",
            requested.display()
        ))));
    }
    Ok(requested)
}

fn env_key(line: &str) -> Option<&str> {
    let trimmed = line.trim_start();
    if trimmed.starts_with('#') || trimmed.is_empty() {
        return None;
    }
    let assignment = trimmed.strip_prefix("export ").unwrap_or(trimmed);
    let separator = assignment.find('=')?;
    Some(assignment[..separator].trim())
}

fn set_env_value(content: &str, key: &str, value: &str) -> String {
    let mut replaced = false;
    let mut lines = content
        .lines()
        .map(|line| {
            if !replaced && env_key(line) == Some(key) {
                replaced = true;
                format!("{key}={value}")
            } else {
                line.to_string()
            }
        })
        .collect::<Vec<_>>();

    if !replaced {
        if !lines.is_empty() && !lines.last().is_some_and(|line| line.is_empty()) {
            lines.push(String::new());
        }
        lines.push(format!("{key}={value}"));
    }

    let mut output = lines.join("\n");
    output.push('\n');
    output
}

fn parse_env_value(raw_value: &str) -> String {
    let value = raw_value.trim();
    if value.len() >= 2 {
        let first = value.as_bytes()[0];
        let last = value.as_bytes()[value.len() - 1];
        if (first == b'"' && last == b'"') || (first == b'\'' && last == b'\'') {
            return value[1..value.len() - 1].to_string();
        }
    }

    let comment_index = value
        .as_bytes()
        .windows(2)
        .position(|window| window[0].is_ascii_whitespace() && window[1] == b'#');
    match comment_index {
        Some(index) => value[..index].trim().to_string(),
        None => value.to_string(),
    }
}

fn get_env_value(content: &str, key: &str) -> Option<String> {
    for line in content.lines() {
        if env_key(line) != Some(key) {
            continue;
        }
        let assignment = line
            .trim_start()
            .strip_prefix("export ")
            .unwrap_or(line.trim_start());
        return Some(parse_env_value(assignment.split_once('=')?.1));
    }
    None
}

fn collect_env_values(content: &str) -> Vec<(String, String)> {
    content
        .lines()
        .filter_map(|line| {
            let key = env_key(line)?.to_string();
            let assignment = line
                .trim_start()
                .strip_prefix("export ")
                .unwrap_or(line.trim_start());
            let raw_value = assignment.split_once('=')?.1;
            Some((key, parse_env_value(raw_value)))
        })
        .collect()
}

struct PreparedConfig {
    host: String,
    port: u16,
    runtime: RuntimeMode,
    environment: Vec<(String, String)>,
}

fn prepare_files(repo: &Path, options: &Options) -> SetupResult<PreparedConfig> {
    let env_path = repo.join(".env");
    let example_path = repo.join(".env.example");
    let env_created = !env_path.exists();

    let mut content = if env_created {
        fs::read_to_string(&example_path).map_err(|error| {
            SetupError(format!(
                "failed to read {}: {error}",
                example_path.display()
            ))
        })?
    } else {
        fs::read_to_string(&env_path).map_err(|error| {
            SetupError(format!("failed to read {}: {error}", env_path.display()))
        })?
    };

    if content.starts_with('\u{feff}') {
        content.remove(0);
    }

    let termux = is_termux_environment();
    if env_created || options.runtime.is_some() {
        let default_runtime = recommended_runtime();
        content = set_env_value(
            &content,
            "DEVBOX_RUNTIME_MODE",
            options.runtime.unwrap_or(default_runtime).as_str(),
        );
    }
    if let Some(host) = &options.bind_host {
        content = set_env_value(&content, "HOST", host);
    }
    if let Some(port) = options.port {
        content = set_env_value(&content, "PORT", &port.to_string());
    }
    if let Some(auth) = options.auth {
        content = set_env_value(&content, "MCP_AUTH_MODE", auth.env_mode());
    }
    if let Some(public_base_url) = &options.public_base_url {
        content = set_env_value(&content, "PUBLIC_BASE_URL", public_base_url.trim());
    }
    if let Some(team_domain) = &options.cloudflare_team_domain {
        content = set_env_value(
            &content,
            "CLOUDFLARE_ACCESS_TEAM_DOMAIN",
            team_domain.trim(),
        );
    }
    if let Some(audience) = &options.cloudflare_aud {
        content = set_env_value(&content, "CLOUDFLARE_ACCESS_AUD", audience.trim());
    }
    if let Some(jwks_url) = &options.cloudflare_jwks_url {
        content = set_env_value(&content, "CLOUDFLARE_ACCESS_JWKS_URL", jwks_url.trim());
    }

    if let Some(auth) = options.auth {
        if auth != AuthChoice::None {
            let public = get_env_value(&content, "PUBLIC_BASE_URL").unwrap_or_default();
            if public.trim().is_empty() {
                return Err(Box::new(SetupError(format!(
                    "--auth {} requires --public-base-url (or an existing PUBLIC_BASE_URL in .env)",
                    auth.as_str()
                ))));
            }
        }
        if auth == AuthChoice::Cloudflare {
            let team_domain =
                get_env_value(&content, "CLOUDFLARE_ACCESS_TEAM_DOMAIN").unwrap_or_default();
            let audience = get_env_value(&content, "CLOUDFLARE_ACCESS_AUD").unwrap_or_default();
            if team_domain.trim().is_empty() || audience.trim().is_empty() {
                return Err(Box::new(SetupError(
                    "--auth cloudflare requires Cloudflare team domain and audience".to_string(),
                )));
            }
            let jwks = get_env_value(&content, "CLOUDFLARE_ACCESS_JWKS_URL").unwrap_or_default();
            if jwks.trim().is_empty() {
                let derived = format!("{}/cdn-cgi/access/certs", team_domain.trim_end_matches('/'));
                content = set_env_value(&content, "CLOUDFLARE_ACCESS_JWKS_URL", &derived);
            }
        }
    }

    let workspace = options
        .workspace
        .as_ref()
        .map(|value| {
            if value.is_absolute() {
                value.clone()
            } else {
                repo.join(value)
            }
        })
        .unwrap_or_else(|| repo.join("workspace"));
    if options.workspace.is_some() || (termux && env_created) {
        let workspace_value = workspace.to_string_lossy();
        content = set_env_value(&content, "HOST_WORKSPACE_PATH", &workspace_value);
        content = set_env_value(&content, "HOST_DEFAULT_WORKDIR", &workspace_value);
    }
    if termux && env_created {
        let shell = env::var("PREFIX")
            .ok()
            .map(|prefix| PathBuf::from(prefix).join("bin/bash"))
            .filter(|path| path.is_file())
            .or_else(|| env::var("SHELL").ok().map(PathBuf::from));
        if let Some(shell) = shell {
            content = set_env_value(&content, "HOST_SHELL", &shell.to_string_lossy());
        }
    }

    if options.dry_run {
        println!("Would write configuration: {}", env_path.display());
        println!("Would create: {}", workspace.display());
        println!("Would create: {}", repo.join("run").display());
    } else {
        fs::write(&env_path, &content)?;
        fs::create_dir_all(&workspace)?;
        fs::create_dir_all(repo.join("run"))?;
    }

    println!(
        "{} {}",
        if env_created { "Created" } else { "Updated" },
        env_path.display()
    );

    let host = get_env_value(&content, "HOST")
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "0.0.0.0".to_string());
    let port = get_env_value(&content, "PORT")
        .and_then(|value| value.parse::<u16>().ok())
        .unwrap_or(8100);
    let runtime = get_env_value(&content, "DEVBOX_RUNTIME_MODE")
        .and_then(|value| RuntimeMode::parse(&value).ok())
        .unwrap_or(RuntimeMode::Auto);

    let environment = collect_env_values(&content);

    Ok(PreparedConfig {
        host,
        port,
        runtime,
        environment,
    })
}

fn warn_if_docker_unavailable(runtime: RuntimeMode) {
    if runtime.resolved() != RuntimeMode::Docker {
        return;
    }
    match capture_command(command_name("docker"), &["version", "--format", "{{.Server.Version}}"], None) {
        Ok(version) => println!("Docker server: {version}"),
        Err(error) => eprintln!(
            "warning: Docker mode is selected but Docker is not ready: {error}\n\
             The MCP service can still be installed; start Docker Desktop before using Docker-backed tools."
        ),
    }
}

fn loopback_host(bind_host: &str) -> &str {
    match bind_host.trim() {
        "" | "0.0.0.0" => "127.0.0.1",
        "::" | "[::]" => "::1",
        other => other.trim_matches(|character| character == '[' || character == ']'),
    }
}

fn format_host_port(host: &str, port: u16) -> String {
    let normalized = host.trim_matches(|character| character == '[' || character == ']');
    if normalized.contains(':') {
        format!("[{normalized}]:{port}")
    } else {
        format!("{normalized}:{port}")
    }
}

fn decode_chunked_http_body(body: &str) -> Option<String> {
    let bytes = body.as_bytes();
    let mut offset = 0usize;
    let mut decoded = Vec::<u8>::new();

    loop {
        let line_end = bytes[offset..]
            .windows(2)
            .position(|window| window == b"\r\n")?
            + offset;
        let size_line = std::str::from_utf8(&bytes[offset..line_end]).ok()?;
        let size_text = size_line.split(';').next()?.trim();
        let size = usize::from_str_radix(size_text, 16).ok()?;
        offset = line_end + 2;

        if size == 0 {
            return String::from_utf8(decoded).ok();
        }
        let chunk_end = offset.checked_add(size)?;
        if chunk_end > bytes.len() || bytes.get(chunk_end..chunk_end + 2)? != b"\r\n" {
            return None;
        }
        decoded.extend_from_slice(&bytes[offset..chunk_end]);
        offset = chunk_end + 2;
    }
}

fn health_response_is_ok(response: &str) -> bool {
    let (headers, body) = match response.split_once("\r\n\r\n") {
        Some(parts) => parts,
        None => return false,
    };
    let status_ok = headers
        .lines()
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        == Some("200");
    if !status_ok {
        return false;
    }

    let chunked = headers.lines().any(|line| {
        let mut parts = line.splitn(2, ':');
        let name = parts.next().unwrap_or_default().trim();
        let value = parts.next().unwrap_or_default();
        name.eq_ignore_ascii_case("transfer-encoding")
            && value
                .split(',')
                .any(|encoding| encoding.trim().eq_ignore_ascii_case("chunked"))
    });
    if chunked {
        return decode_chunked_http_body(body).is_some_and(|decoded| decoded.trim() == "ok");
    }
    body.trim() == "ok"
}

fn health_check(host: &str, port: u16) -> bool {
    let address = format_host_port(host, port);
    let socket = match address
        .to_socket_addrs()
        .ok()
        .and_then(|mut values| values.next())
    {
        Some(socket) => socket,
        None => return false,
    };
    let mut stream = match TcpStream::connect_timeout(&socket, Duration::from_millis(300)) {
        Ok(stream) => stream,
        Err(_) => return false,
    };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let request_host = format_host_port(host, port);
    let request =
        format!("GET /healthz HTTP/1.1\r\nHost: {request_host}\r\nConnection: close\r\n\r\n");
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }
    let mut response = String::new();
    if stream.read_to_string(&mut response).is_err() {
        return false;
    }
    health_response_is_ok(&response)
}

fn wait_for_health(host: &str, port: u16) -> bool {
    for _ in 0..50 {
        if health_check(host, port) {
            return true;
        }
        thread::sleep(Duration::from_millis(100));
    }
    false
}

fn install_guardian(repo: &Path, runtime: RuntimeMode, options: &Options) -> SetupResult<()> {
    if !options.install_guardian {
        return Ok(());
    }
    println!("Installing Guardian reliability supervision...");
    match platform_kind() {
        PlatformKind::Windows => {
            let shell = first_available_command(
                &["pwsh.exe", "pwsh", "powershell.exe", "powershell"],
                &[
                    "-NoLogo",
                    "-NoProfile",
                    "-Command",
                    "$PSVersionTable.PSVersion.ToString()",
                ],
            )
            .ok_or_else(|| {
                Box::new(SetupError(
                    "PowerShell is required to install the Windows Guardian tasks".to_string(),
                )) as Box<dyn Error>
            })?;
            let script = repo
                .join("scripts/Install-ChatGptDevboxGuardian.ps1")
                .to_string_lossy()
                .to_string();
            let args = vec![
                "-NoLogo".into(),
                "-NoProfile".into(),
                "-NonInteractive".into(),
                "-ExecutionPolicy".into(),
                "Bypass".into(),
                "-File".into(),
                script,
                "-Runtime".into(),
                runtime.as_str().into(),
            ];
            run_owned_command(&shell, &args, Some(repo), options.dry_run)?;
        }
        PlatformKind::Termux | PlatformKind::Linux | PlatformKind::MacOS => {
            let mode = match platform_kind() {
                PlatformKind::Termux => "termux",
                PlatformKind::MacOS => "launchd",
                _ => "auto",
            };
            let script = repo
                .join("scripts/install-guardian.sh")
                .to_string_lossy()
                .to_string();
            run_command("sh", &[&script, mode], Some(repo), options.dry_run)?;
        }
        PlatformKind::Other => {
            eprintln!("warning: Guardian service installation is not available on this platform");
        }
    }
    Ok(())
}

fn setup(options: Options) -> SetupResult<()> {
    println!("Devbox MCP setup {VERSION}");
    println!("Platform: {}", platform_kind().as_str());
    install_system_prerequisites(&options)?;
    refresh_common_tool_paths();
    ensure_supported_rust_toolchain(&options)?;
    refresh_common_tool_paths();
    verify_toolchain()?;
    let repo = clone_or_locate_repo(&options)?;
    if options.dry_run && !is_devbox_repo(&repo) {
        println!("Dry run complete; the repository would be cloned before configuration.");
        return Ok(());
    }
    let prepared = prepare_files(&repo, &options)?;
    warn_if_docker_unavailable(prepared.runtime);
    if options.auth.is_some_and(|auth| auth != AuthChoice::None) {
        match capture_command("cloudflared", &["--version"], None) {
            Ok(version) => {
                println!("cloudflared: {version}");
                println!(
                    "Cloudflare Tunnel setup: {}",
                    cloudflare_transport_next_step(platform_kind())
                );
            }
            Err(_) => eprintln!(
                "note: cloudflared is not installed. Authentication still works behind another HTTPS reverse proxy.\nIf you want Cloudflare Tunnel transport on this platform, install it with:\n{}\nThen: {}\nFull guide: docs/CLOUDFLARE_TUNNEL.md",
                cloudflared_install_hint(),
                cloudflare_transport_next_step(platform_kind()),
            ),
        }
    }

    if options.install_dependencies {
        run_command(
            command_name("npm"),
            &["install"],
            Some(&repo),
            options.dry_run,
        )?;
    }
    if options.link_command {
        if let Err(error) =
            run_command(command_name("npm"), &["link"], Some(&repo), options.dry_run)
        {
            eprintln!(
                "warning: npm link failed: {error}\nThe service will still work through `node bin/devbox.js`; rerun npm link later if you want the global `devbox` command."
            );
        }
    }
    if options.start_server {
        run_command_with_environment(
            command_name("node"),
            &["bin/devbox.js", "start"],
            Some(&repo),
            options.dry_run,
            &prepared.environment,
        )?;

        if !options.dry_run {
            let host = loopback_host(&prepared.host);
            if !wait_for_health(host, prepared.port) {
                return Err(Box::new(SetupError(format!(
                    "Devbox started but health check failed at http://{host}:{}/healthz; inspect {}",
                    prepared.port,
                    repo.join("run/devbox.log").display()
                ))));
            }
            println!("Health check: ok");
        }
    }

    if options.install_guardian {
        if let Err(error) = install_guardian(&repo, prepared.runtime, &options) {
            eprintln!("warning: Devbox is installed, but Guardian setup failed: {error}");
            eprintln!("You can rerun Guardian installation later from the scripts directory.");
        }
    }

    let host = loopback_host(&prepared.host);
    println!();
    println!("Setup complete.");
    println!("Repository: {}", repo.display());
    println!("Platform: {}", platform_kind().as_str());
    if is_termux_environment() {
        println!("Canonical Termux app: {CANONICAL_TERMUX_REPO}");
    }
    println!(
        "Runtime: {} (resolved: {})",
        prepared.runtime.as_str(),
        prepared.runtime.resolved().as_str()
    );
    println!("MCP URL: http://{host}:{}/mcp", prepared.port);
    println!("Health URL: http://{host}:{}/healthz", prepared.port);
    if let Some(auth) = options.auth {
        println!("Authentication: {}", auth.as_str());
    }
    println!("Commands: devbox status | devbox restart | devbox stop | devbox run");
    Ok(())
}

fn main() {
    let options = match parse_args(env::args().skip(1)) {
        Ok(options) => options,
        Err(error) => {
            eprintln!("error: {error}");
            std::process::exit(2);
        }
    };

    if let Err(error) = setup(options) {
        eprintln!("setup failed: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_node_major_versions() {
        assert_eq!(parse_node_major("v22.22.2"), Some(22));
        assert_eq!(parse_node_major("18.20.0"), Some(18));
        assert_eq!(parse_node_major("unknown"), None);
    }

    #[test]
    fn parses_and_checks_rust_versions() {
        assert_eq!(
            parse_rust_version("rustc 1.97.1 (abc 2026-07-14)"),
            Some((1, 97, 1))
        );
        assert_eq!(parse_rust_version("rustc 1.88.0"), Some((1, 88, 0)));
        assert!(rust_version_is_supported("rustc 1.88.0"));
        assert!(rust_version_is_supported("rustc 1.97.1"));
        assert!(!rust_version_is_supported("rustc 1.85.0"));
        assert_eq!(parse_rust_version("unknown"), None);
    }

    #[test]
    fn parses_runtime_modes() {
        assert_eq!(RuntimeMode::parse("AUTO").unwrap(), RuntimeMode::Auto);
        assert_eq!(RuntimeMode::parse("host").unwrap(), RuntimeMode::Host);
        assert_eq!(RuntimeMode::parse("docker").unwrap(), RuntimeMode::Docker);
        assert!(RuntimeMode::parse("vm").is_err());
    }

    #[test]
    fn detects_termux_from_version_or_prefix() {
        assert!(is_termux_values(Some("0.118"), None));
        assert!(is_termux_values(
            None,
            Some("/data/data/com.termux/files/usr")
        ));
        assert!(!is_termux_values(None, Some("/usr")));
        assert!(!is_termux_values(Some(""), None));
    }

    #[test]
    fn termux_packages_include_required_runtime_tools() {
        let arguments = termux_package_arguments();
        assert_eq!(&arguments[..2], &["install", "-y"]);
        for package in ["nodejs", "git", "python", "ripgrep", "curl", "rust"] {
            assert!(arguments.contains(&package));
        }
    }

    #[test]
    fn updates_existing_environment_values_without_losing_comments() {
        let source = "# keep this\nPORT=8100\nHOST=0.0.0.0\n";
        let updated = set_env_value(source, "PORT", "9000");
        assert!(updated.contains("# keep this"));
        assert!(updated.contains("PORT=9000"));
        assert_eq!(updated.matches("PORT=").count(), 1);
        assert_eq!(get_env_value(&updated, "PORT").as_deref(), Some("9000"));
    }

    #[test]
    fn appends_missing_environment_values() {
        let updated = set_env_value("HOST=127.0.0.1\n", "PORT", "8100");
        assert!(updated.contains("HOST=127.0.0.1"));
        assert!(updated.contains("PORT=8100"));
    }

    #[test]
    fn collects_environment_values_for_child_processes() {
        let values = collect_env_values(
            "MCP_AUTH_MODE=none\nPUBLIC_BASE_URL=\nPORT=18142\nINLINE=value # comment\n",
        );
        assert!(values.contains(&("MCP_AUTH_MODE".to_string(), "none".to_string())));
        assert!(values.contains(&("PUBLIC_BASE_URL".to_string(), String::new())));
        assert!(values.contains(&("PORT".to_string(), "18142".to_string())));
        assert!(values.contains(&("INLINE".to_string(), "value".to_string())));
    }

    #[test]
    fn rejects_port_zero() {
        let result = parse_args(["--port".to_string(), "0".to_string()]);
        assert!(result.is_err());
    }

    #[test]
    fn parses_cli_options() {
        let options = parse_args([
            "--repo".to_string(),
            "sample".to_string(),
            "--runtime".to_string(),
            "host".to_string(),
            "--port".to_string(),
            "9000".to_string(),
            "--auth".to_string(),
            "oauth".to_string(),
            "--public-base-url".to_string(),
            "https://mcp.example.test".to_string(),
            "--skip-system-packages".to_string(),
            "--no-start".to_string(),
            "--guardian".to_string(),
        ])
        .unwrap();
        assert_eq!(options.repo, Some(PathBuf::from("sample")));
        assert_eq!(options.runtime, Some(RuntimeMode::Host));
        assert_eq!(options.port, Some(9000));
        assert_eq!(options.auth, Some(AuthChoice::OAuth));
        assert_eq!(
            options.public_base_url.as_deref(),
            Some("https://mcp.example.test")
        );
        assert!(!options.install_system_packages);
        assert!(!options.start_server);
        assert!(options.install_guardian);
    }

    #[test]
    fn maps_user_facing_auth_choices_to_server_modes() {
        assert_eq!(AuthChoice::parse("none").unwrap().env_mode(), "none");
        assert_eq!(AuthChoice::parse("oauth").unwrap().env_mode(), "demo-oauth");
        assert_eq!(
            AuthChoice::parse("cloudflare").unwrap().env_mode(),
            "cloudflare-access"
        );
        assert!(AuthChoice::parse("invalid").is_err());
    }

    #[test]
    fn cloudflared_install_hints_are_platform_specific() {
        assert!(
            cloudflared_install_hint_for(PlatformKind::Windows, None).contains("winget install")
        );
        assert!(cloudflared_install_hint_for(PlatformKind::MacOS, None)
            .contains("brew install cloudflared"));
        assert!(cloudflared_install_hint_for(PlatformKind::Termux, None)
            .contains("pkg install cloudflared termux-services"));
        assert!(
            cloudflared_install_hint_for(PlatformKind::Linux, Some("apt-get"))
                .contains("pkg.cloudflare.com/cloudflare-main.gpg")
        );
        assert!(
            cloudflared_install_hint_for(PlatformKind::Linux, Some("dnf"))
                .contains("cloudflared.repo")
        );
        assert!(
            cloudflared_install_hint_for(PlatformKind::Linux, Some("pacman"))
                .contains("pacman -Syu cloudflared")
        );
        assert!(
            cloudflared_install_hint_for(PlatformKind::Linux, Some("apk"))
                .contains("does not document an apk repository")
        );
    }

    #[test]
    fn recommends_host_without_docker_and_preserves_windows_auto_with_docker() {
        assert_eq!(
            recommended_runtime_for(PlatformKind::Windows, false),
            RuntimeMode::Host
        );
        assert_eq!(
            recommended_runtime_for(PlatformKind::Windows, true),
            RuntimeMode::Auto
        );
        assert_eq!(
            recommended_runtime_for(PlatformKind::Linux, true),
            RuntimeMode::Host
        );
        assert_eq!(
            recommended_runtime_for(PlatformKind::MacOS, false),
            RuntimeMode::Host
        );
        assert_eq!(
            recommended_runtime_for(PlatformKind::Termux, false),
            RuntimeMode::Host
        );
    }

    #[test]
    fn normalizes_wildcard_bind_hosts() {
        assert_eq!(loopback_host("0.0.0.0"), "127.0.0.1");
        assert_eq!(loopback_host("::"), "::1");
        assert_eq!(loopback_host("localhost"), "localhost");
    }

    #[test]
    fn formats_ipv4_and_ipv6_socket_addresses() {
        assert_eq!(format_host_port("127.0.0.1", 8100), "127.0.0.1:8100");
        assert_eq!(format_host_port("::1", 8100), "[::1]:8100");
        assert_eq!(format_host_port("[::1]", 8100), "[::1]:8100");
    }

    #[test]
    fn accepts_content_length_and_chunked_health_responses() {
        let content_length = concat!(
            "HTTP/1.1 200 OK\r\n",
            "content-type: text/plain\r\n",
            "content-length: 2\r\n",
            "connection: close\r\n\r\n",
            "ok"
        );
        let chunked = concat!(
            "HTTP/1.1 200 OK\r\n",
            "content-type: text/plain; charset=utf-8\r\n",
            "transfer-encoding: chunked\r\n",
            "connection: close\r\n\r\n",
            "2\r\nok\r\n0\r\n\r\n"
        );
        assert!(health_response_is_ok(content_length));
        assert!(health_response_is_ok(chunked));
    }

    #[test]
    fn rejects_malformed_or_unhealthy_http_responses() {
        assert!(!health_response_is_ok(
            "HTTP/1.1 503 Service Unavailable\r\ncontent-length: 2\r\n\r\nok"
        ));
        assert!(!health_response_is_ok(
            "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n3\r\nbad\r\n0\r\n\r\n"
        ));
        assert!(!health_response_is_ok(
            "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n2\r\no"
        ));
    }
}
