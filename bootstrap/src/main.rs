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
const TERMUX_PACKAGES: &[&str] = &[
    "nodejs",
    "git",
    "python",
    "ripgrep",
    "curl",
    "ca-certificates",
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
    install_system_packages: bool,
    install_dependencies: bool,
    link_command: bool,
    start_server: bool,
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
            install_system_packages: true,
            install_dependencies: true,
            link_command: true,
            start_server: true,
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
    --skip-system-packages Do not install Termux packages with pkg
    --skip-install         Do not run npm install
    --no-link              Do not run npm link
    --no-start             Do not start the MCP service
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
            "--skip-system-packages" => options.install_system_packages = false,
            "--skip-install" => options.install_dependencies = false,
            "--no-link" => options.link_command = false,
            "--no-start" => options.start_server = false,
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

fn is_termux_values(termux_version: Option<&str>, prefix: Option<&str>) -> bool {
    termux_version.is_some_and(|value| !value.trim().is_empty())
        || prefix.is_some_and(|value| value.contains("com.termux/files/usr"))
}

fn is_termux_environment() -> bool {
    let termux_version = env::var("TERMUX_VERSION").ok();
    let prefix = env::var("PREFIX").ok();
    is_termux_values(termux_version.as_deref(), prefix.as_deref())
}

fn termux_package_arguments() -> Vec<&'static str> {
    let mut arguments = vec!["install", "-y"];
    arguments.extend_from_slice(TERMUX_PACKAGES);
    arguments
}

fn install_termux_prerequisites(options: &Options) -> SetupResult<()> {
    if !is_termux_environment() || !options.install_system_packages {
        return Ok(());
    }

    println!("Termux on Android detected.");
    println!("Canonical Termux app: {CANONICAL_TERMUX_REPO}");
    let arguments = termux_package_arguments();
    run_command("pkg", &arguments, None, options.dry_run).map_err(|error| {
        Box::new(SetupError(format!(
            "failed to install Termux packages with pkg: {error}. Use --skip-system-packages only when Node.js 18+, npm, and Git are already installed"
        ))) as Box<dyn Error>
    })
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
    println!("Node.js: {node}");
    println!("npm: {npm}");
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
        let default_runtime = if termux {
            RuntimeMode::Host
        } else {
            RuntimeMode::Auto
        };
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
    let (headers, body) = response.split_once("\r\n\r\n").unwrap_or((&response, ""));
    headers.starts_with("HTTP/1.1 200") && body.trim() == "ok"
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

fn setup(options: Options) -> SetupResult<()> {
    println!("Devbox MCP setup {VERSION}");
    install_termux_prerequisites(&options)?;
    verify_toolchain()?;
    let repo = clone_or_locate_repo(&options)?;
    if options.dry_run && !is_devbox_repo(&repo) {
        println!("Dry run complete; the repository would be cloned before configuration.");
        return Ok(());
    }
    let prepared = prepare_files(&repo, &options)?;
    warn_if_docker_unavailable(prepared.runtime);

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

    let host = loopback_host(&prepared.host);
    println!();
    println!("Setup complete.");
    println!("Repository: {}", repo.display());
    if is_termux_environment() {
        println!("Platform: Termux on Android");
        println!("Canonical Termux app: {CANONICAL_TERMUX_REPO}");
    }
    println!(
        "Runtime: {} (resolved: {})",
        prepared.runtime.as_str(),
        prepared.runtime.resolved().as_str()
    );
    println!("MCP URL: http://{host}:{}/mcp", prepared.port);
    println!("Health URL: http://{host}:{}/healthz", prepared.port);
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
        for package in ["nodejs", "git", "python", "ripgrep", "curl"] {
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
            "--skip-system-packages".to_string(),
            "--no-start".to_string(),
        ])
        .unwrap();
        assert_eq!(options.repo, Some(PathBuf::from("sample")));
        assert_eq!(options.runtime, Some(RuntimeMode::Host));
        assert_eq!(options.port, Some(9000));
        assert!(!options.install_system_packages);
        assert!(!options.start_server);
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
}
