#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.4.2";
constexpr const char* kRepoUrl = "https://github.com/adybag14-cyber/devbox.git";

struct Theme {
    bool color = true;
    const char* reset = "\x1b[0m";
    const char* bold = "\x1b[1m";
    const char* dim = "\x1b[2m";
    const char* cyan = "\x1b[36m";
    const char* green = "\x1b[32m";
    const char* yellow = "\x1b[33m";
    const char* red = "\x1b[31m";
    const char* blue = "\x1b[34m";

    std::string paint(const char* code, const std::string& text) const {
        return color ? std::string(code) + text + reset : text;
    }
};

struct PlatformInfo {
    std::string os;
    std::string arch;
    bool termux = false;
};

struct ToolStatus {
    std::string name;
    bool available = false;
    std::string version;
    bool required = false;
};

enum class RuntimeChoice { Auto, Host, Docker };
enum class AuthChoice { None, OAuth, Cloudflare };

struct SetupConfig {
    fs::path repo_path;
    RuntimeChoice runtime = RuntimeChoice::Host;
    AuthChoice auth = AuthChoice::None;
    std::string public_base_url;
    std::string cloudflare_team_domain;
    std::string cloudflare_aud;
    std::string cloudflare_jwks_url;
    std::string host = "127.0.0.1";
    int port = 8100;
    std::optional<fs::path> workspace;
    bool install_system_packages = true;
    bool install_dependencies = true;
    bool link_command = true;
    bool start_server = true;
    bool install_guardian = true;
    bool dry_run = false;
};

bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

void enable_virtual_terminal() {
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return;
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) return;
    SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

std::string getenv_string(const char* key) {
    const char* value = std::getenv(key);
    return value ? std::string(value) : std::string();
}

PlatformInfo detect_platform() {
    PlatformInfo info;
#ifdef _WIN32
    info.os = "Windows";
#elif defined(__APPLE__)
    info.os = "macOS";
#elif defined(__ANDROID__)
    info.os = "Android";
#elif defined(__linux__)
    info.os = "Linux";
#else
    info.os = "Unknown";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    info.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    info.arch = "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    info.arch = "armv7";
#elif defined(__i386__) || defined(_M_IX86)
    info.arch = "x86";
#else
    info.arch = "unknown";
#endif

    const std::string prefix = getenv_string("PREFIX");
    info.termux = !getenv_string("TERMUX_VERSION").empty() || prefix.find("com.termux/files/usr") != std::string::npos;
    if (info.termux) info.os = "Termux / Android";
    return info;
}

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char ch : value) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out.push_back('\'');
    return out;
#endif
}

std::string capture_command(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) return {};
    std::string output;
    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
#ifdef _WIN32
    const int code = _pclose(pipe);
#else
    const int code = pclose(pipe);
#endif
    if (code != 0) return {};
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

bool command_available(const std::string& command, const std::string& version_arg = "--version") {
    return !capture_command(shell_quote(command) + " " + version_arg).empty();
}

bool command_exists(const std::string& command) {
#ifdef _WIN32
    return !capture_command("where " + shell_quote(command)).empty();
#else
    return !capture_command("command -v " + shell_quote(command)).empty();
#endif
}

std::string first_line(std::string value) {
    const auto pos = value.find_first_of("\r\n");
    if (pos != std::string::npos) value.resize(pos);
    return value;
}

ToolStatus probe_tool(const std::string& name, const std::string& command, bool required, const std::string& args = "--version") {
    const std::string output = capture_command(shell_quote(command) + " " + args);
    return ToolStatus{name, !output.empty(), first_line(output), required};
}

std::string package_manager(const PlatformInfo& platform) {
    if (platform.termux) return command_exists("pkg") ? "pkg" : "not found";
#ifdef _WIN32
    if (command_available("winget", "--version")) return "winget";
    if (command_available("choco", "--version")) return "chocolatey";
    return "not found";
#elif defined(__APPLE__)
    if (command_available("brew", "--version")) return "Homebrew";
    return "not found";
#else
    for (const auto& item : {"apt-get", "dnf", "yum", "pacman", "zypper", "apk"}) {
        if (command_exists(item)) return item;
    }
    return "not found";
#endif
}

fs::path executable_path() {
#ifdef _WIN32
    std::vector<char> buffer(32768);
    const DWORD length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) return fs::path(std::string(buffer.data(), length));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) return fs::weakly_canonical(fs::path(buffer.data()));
#else
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) return fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
    return fs::current_path() / "devbox-tui";
}

std::optional<std::string> locate_bootstrap(const std::optional<std::string>& explicit_path = std::nullopt) {
    if (explicit_path && !explicit_path->empty()) {
        if (fs::exists(*explicit_path)) return *explicit_path;
        return std::nullopt;
    }
#ifdef _WIN32
    const char* filename = "devbox-setup.exe";
#else
    const char* filename = "devbox-setup";
#endif
    const fs::path executable_dir = executable_path().parent_path();
    const fs::path sibling = executable_dir / filename;
    if (fs::exists(sibling)) return sibling.string();
    try {
        std::vector<fs::path> named_release_candidates;
        for (const auto& entry : fs::directory_iterator(executable_dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
#ifdef _WIN32
            if (name.rfind("devbox-setup-", 0) == 0 && entry.path().extension() == ".exe") {
                named_release_candidates.push_back(entry.path());
            }
#else
            if (name.rfind("devbox-setup-", 0) == 0) {
                named_release_candidates.push_back(entry.path());
            }
#endif
        }
        std::sort(named_release_candidates.begin(), named_release_candidates.end());
        if (!named_release_candidates.empty()) return named_release_candidates.front().string();
    } catch (...) {
    }
    if (command_available(filename, "--version")) return std::string(filename);
    const fs::path repo_build = fs::current_path() / "bootstrap" / "target" / "release" / filename;
    if (fs::exists(repo_build)) return repo_build.string();
    const fs::path repo_debug = fs::current_path() / "bootstrap" / "target" / "debug" / filename;
    if (fs::exists(repo_debug)) return repo_debug.string();
    return std::nullopt;
}

int run_process(const std::vector<std::string>& args) {
    if (args.empty()) return 127;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& item : args) argv.push_back(const_cast<char*>(item.c_str()));
    argv.push_back(nullptr);
#ifdef _WIN32
    const intptr_t code = _spawnvp(_P_WAIT, args[0].c_str(), argv.data());
    if (code == -1) {
        std::cerr << "Failed to start " << args[0] << ": errno=" << errno << "\n";
        return 127;
    }
    return static_cast<int>(code);
#else
    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork failed: errno=" << errno << "\n";
        return 127;
    }
    if (pid == 0) {
        execvp(args[0].c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

void clear_screen() {
    if (stdout_is_tty()) std::cout << "\x1b[2J\x1b[H";
}

void rule(const Theme& theme, char ch = '-') {
    std::cout << theme.paint(theme.dim, std::string(72, ch)) << "\n";
}

void header(const Theme& theme, const PlatformInfo& platform) {
    std::cout << theme.paint(theme.cyan, theme.paint(theme.bold, "DEVBOX SETUP TUI")) << "  "
              << theme.paint(theme.dim, "v" + std::string(kVersion)) << "\n";
    std::cout << "Native guided installer for " << theme.paint(theme.green, platform.os)
              << " / " << platform.arch << "\n";
    rule(theme, '=');
}

std::string prompt_text(const std::string& label, const std::string& fallback) {
    std::cout << label;
    if (!fallback.empty()) std::cout << " [" << fallback << "]";
    std::cout << ": ";
    std::string value;
    std::getline(std::cin, value);
    return value.empty() ? fallback : value;
}

std::string prompt_required_text(const std::string& label, const std::string& fallback = "") {
    while (true) {
        const std::string value = prompt_text(label, fallback);
        if (!value.empty()) return value;
        std::cout << "A value is required for this authentication mode.\n";
    }
}

bool prompt_yes_no(const std::string& label, bool fallback) {
    while (true) {
        std::cout << label << (fallback ? " [Y/n]: " : " [y/N]: ");
        std::string value;
        std::getline(std::cin, value);
        if (value.empty()) return fallback;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value == "y" || value == "yes") return true;
        if (value == "n" || value == "no") return false;
    }
}

int prompt_port(int fallback) {
    while (true) {
        const std::string value = prompt_text("MCP port", std::to_string(fallback));
        try {
            const int port = std::stoi(value);
            if (port >= 1 && port <= 65535) return port;
        } catch (...) {
        }
        std::cout << "Enter a port from 1 to 65535.\n";
    }
}

std::size_t menu(const Theme& theme, const std::string& title, const std::vector<std::string>& items, std::size_t fallback) {
    std::cout << theme.paint(theme.bold, title) << "\n";
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << items[i];
        if (i == fallback) std::cout << theme.paint(theme.dim, "  [recommended]");
        std::cout << "\n";
    }
    while (true) {
        const std::string value = prompt_text("Choose", std::to_string(fallback + 1));
        try {
            const auto selected = static_cast<std::size_t>(std::stoul(value));
            if (selected >= 1 && selected <= items.size()) return selected - 1;
        } catch (...) {
        }
        std::cout << "Choose one of the numbered options.\n";
    }
}

std::string runtime_name(RuntimeChoice runtime) {
    switch (runtime) {
        case RuntimeChoice::Auto: return "auto";
        case RuntimeChoice::Host: return "host";
        case RuntimeChoice::Docker: return "docker";
    }
    return "host";
}

std::string auth_name(AuthChoice auth) {
    switch (auth) {
        case AuthChoice::None: return "none";
        case AuthChoice::OAuth: return "oauth";
        case AuthChoice::Cloudflare: return "cloudflare";
    }
    return "none";
}

bool looks_like_repo(const fs::path& path) {
    return fs::exists(path / "package.json") && fs::exists(path / ".env.example") && fs::exists(path / "src" / "server.js");
}

void print_tool_table(const Theme& theme, const std::vector<ToolStatus>& tools) {
    std::cout << theme.paint(theme.bold, "Preflight") << "\n";
    for (const auto& tool : tools) {
        const std::string marker = tool.available ? theme.paint(theme.green, "OK  ")
                                                  : (tool.required ? theme.paint(theme.red, "MISS") : theme.paint(theme.yellow, "OPT "));
        std::cout << "  " << marker << "  " << tool.name;
        if (tool.available) std::cout << "  " << theme.paint(theme.dim, tool.version);
        std::cout << "\n";
    }
}

std::vector<ToolStatus> collect_tools(const PlatformInfo& platform) {
#ifdef _WIN32
    (void)platform;
#endif
    std::vector<ToolStatus> tools;
    tools.push_back(probe_tool("Node.js", "node", true));
#ifdef _WIN32
    tools.push_back(probe_tool("npm", "npm.cmd", true));
#else
    tools.push_back(probe_tool("npm", "npm", true));
#endif
    tools.push_back(probe_tool("Git", "git", true));
    tools.push_back(probe_tool("Docker", "docker", false));
    tools.push_back(probe_tool("cloudflared", "cloudflared", false));
#ifdef _WIN32
    const std::string pwsh = fs::exists("C:\\Program Files\\PowerShell\\7\\pwsh.exe") ? "C:\\Program Files\\PowerShell\\7\\pwsh.exe" : "pwsh";
    tools.push_back(probe_tool("PowerShell 7", pwsh, false, "-Version"));
#else
    if (!platform.termux) tools.push_back(probe_tool("curl", "curl", false));
#endif
    return tools;
}

bool has_tool(const std::vector<ToolStatus>& tools, const std::string& name) {
    const auto it = std::find_if(tools.begin(), tools.end(), [&](const ToolStatus& item) { return item.name == name; });
    return it != tools.end() && it->available;
}

std::string cloudflared_install_hint(const PlatformInfo& platform) {
    if (platform.termux) return "pkg update && pkg install cloudflared termux-services";
#ifdef _WIN32
    return "winget install --id Cloudflare.cloudflared --exact";
#elif defined(__APPLE__)
    return "brew install cloudflared";
#else
    const std::string manager = package_manager(platform);
    if (manager == "apt-get") {
        return "sudo mkdir -p --mode=0755 /usr/share/keyrings\n"
               "curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg | sudo tee /usr/share/keyrings/cloudflare-main.gpg >/dev/null\n"
               "echo \"deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main\" | sudo tee /etc/apt/sources.list.d/cloudflared.list\n"
               "sudo apt-get update && sudo apt-get install cloudflared";
    }
    if (manager == "dnf" || manager == "yum") {
        return "curl -fsSL https://pkg.cloudflare.com/cloudflared.repo | sudo tee /etc/yum.repos.d/cloudflared.repo\n"
               "sudo dnf install cloudflared  # use yum on yum-based systems";
    }
    if (manager == "pacman") return "sudo pacman -Syu cloudflared";
    if (manager == "apk") {
        return "Alpine: install the matching official cloudflared Linux binary and put it on PATH; see docs/CLOUDFLARE_TUNNEL.md";
    }
    return "Install the matching official cloudflared package/binary and put it on PATH; see docs/CLOUDFLARE_TUNNEL.md";
#endif
}

void print_cloudflare_help(const Theme& theme, const PlatformInfo& platform) {
    header(theme, platform);
    std::cout << theme.paint(theme.bold, "Cloudflare Tunnel transport") << "\n"
              << "Authentication and tunnel transport are separate. Cloudflare Tunnel publishes the local Devbox origin without opening an inbound port.\n\n"
              << theme.paint(theme.bold, "Install cloudflared") << "\n"
              << cloudflared_install_hint(platform) << "\n\n"
              << theme.paint(theme.bold, "Create the tunnel") << "\n"
              << "  Cloudflare Zero Trust -> Networks -> Tunnels -> create a Cloudflared tunnel\n"
              << "  Add a public hostname such as mcp.example.com\n"
              << "  Route it to http://127.0.0.1:8100 (or your configured Devbox port)\n"
              << "  Copy the connector tunnel token\n\n"
              << theme.paint(theme.bold, "Configure Devbox .env") << "\n"
              << "  CLOUDFLARED_TUNNEL_TOKEN=<secret token>\n"
              << "  CLOUDFLARED_PUBLIC_HOSTNAME=mcp.example.com\n"
              << "  PUBLIC_BASE_URL=https://mcp.example.com\n\n";
#ifdef _WIN32
    std::cout << "Restart Devbox; Windows host mode starts the configured named tunnel automatically.\n";
#else
    if (platform.termux) {
        std::cout << "Install persistent Termux service:\n"
                  << "  sh scripts/install-cloudflare-tunnel.sh termux\n"
                  << "  sv status devbox-cloudflared\n";
    } else {
        std::cout << "Install persistent tunnel service:\n"
                  << "  sh scripts/install-cloudflare-tunnel.sh auto\n";
    }
#endif
    std::cout << "\nFull guide: docs/CLOUDFLARE_TUNNEL.md\n"
              << "Online: https://github.com/adybag14-cyber/devbox/blob/main/docs/CLOUDFLARE_TUNNEL.md\n";
}

void print_diagnostics(const Theme& theme, const PlatformInfo& platform) {
    header(theme, platform);
    const auto tools = collect_tools(platform);
    print_tool_table(theme, tools);
    std::cout << "\nPackage manager: " << package_manager(platform) << "\n";
    std::cout << "Repository URL: " << kRepoUrl << "\n";
    const auto bootstrap = locate_bootstrap();
    std::cout << "Rust bootstrap: " << (bootstrap ? *bootstrap : "not found") << "\n";
}

std::vector<std::string> build_bootstrap_args(const std::string& bootstrap, const SetupConfig& config) {
    std::vector<std::string> args{bootstrap, "--repo", config.repo_path.string(), "--runtime", runtime_name(config.runtime),
                                  "--auth", auth_name(config.auth), "--host", config.host, "--port", std::to_string(config.port)};
    if (!config.public_base_url.empty()) {
        args.emplace_back("--public-base-url");
        args.push_back(config.public_base_url);
    }
    if (config.auth == AuthChoice::Cloudflare) {
        args.emplace_back("--cloudflare-team-domain");
        args.push_back(config.cloudflare_team_domain);
        args.emplace_back("--cloudflare-aud");
        args.push_back(config.cloudflare_aud);
        args.emplace_back("--cloudflare-jwks-url");
        args.push_back(config.cloudflare_jwks_url);
    }
    if (config.workspace && !config.workspace->empty()) {
        args.emplace_back("--workspace");
        args.push_back(config.workspace->string());
    }
    if (!config.install_system_packages) args.emplace_back("--skip-system-packages");
    if (!config.install_dependencies) args.emplace_back("--skip-install");
    if (!config.link_command) args.emplace_back("--no-link");
    if (!config.start_server) args.emplace_back("--no-start");
    if (config.install_guardian) args.emplace_back("--guardian");
    if (config.dry_run) args.emplace_back("--dry-run");
    return args;
}

void print_plan(const Theme& theme, const SetupConfig& config) {
    std::cout << theme.paint(theme.bold, "Setup plan") << "\n";
    std::cout << "  Repository       " << config.repo_path.string() << "\n";
    std::cout << "  Runtime          " << runtime_name(config.runtime) << "\n";
    std::cout << "  Authentication   " << auth_name(config.auth) << "\n";
    if (!config.public_base_url.empty()) std::cout << "  Public base URL  " << config.public_base_url << "\n";
    if (config.auth == AuthChoice::Cloudflare) {
        std::cout << "  CF team domain   " << config.cloudflare_team_domain << "\n";
        std::cout << "  CF audience      " << config.cloudflare_aud << "\n";
        std::cout << "  CF JWKS URL      " << config.cloudflare_jwks_url << "\n";
    }
    std::cout << "  Bind             " << config.host << ':' << config.port << "\n";
    std::cout << "  Workspace        " << (config.workspace ? config.workspace->string() : "<repo>/workspace") << "\n";
    std::cout << "  System packages  " << (config.install_system_packages ? "install missing" : "do not install") << "\n";
    std::cout << "  npm install       " << (config.install_dependencies ? "yes" : "no") << "\n";
    std::cout << "  npm link          " << (config.link_command ? "yes" : "no") << "\n";
    std::cout << "  Start MCP         " << (config.start_server ? "yes" : "no") << "\n";
    std::cout << "  Guardian          " << (config.install_guardian ? "install" : "skip") << "\n";
    if (config.dry_run) std::cout << "  Mode              DRY RUN\n";
}

int interactive_setup(const Theme& theme, const PlatformInfo& platform, const std::optional<std::string>& explicit_bootstrap, bool dry_run) {
    clear_screen();
    header(theme, platform);
    auto tools = collect_tools(platform);
    print_tool_table(theme, tools);
    std::cout << "  Package manager: " << package_manager(platform) << "\n\n";

    auto bootstrap = locate_bootstrap(explicit_bootstrap);
    if (!bootstrap) {
        std::cerr << theme.paint(theme.red, "Rust bootstrap binary not found.") << "\n"
                  << "Keep devbox-tui beside devbox-setup in the release bundle, or pass --bootstrap <path>.\n";
        return 2;
    }
    std::cout << "Bootstrap: " << *bootstrap << "\n\n";

    SetupConfig config;
    config.dry_run = dry_run;

    const fs::path cwd = fs::current_path();
    const bool current_repo = looks_like_repo(cwd);
    const auto repo_mode = menu(theme, "Repository", current_repo
        ? std::vector<std::string>{"Use current Devbox checkout", "Use another existing checkout", "Clone a fresh checkout"}
        : std::vector<std::string>{"Clone a fresh checkout", "Use an existing checkout"}, 0);

    if (current_repo && repo_mode == 0) {
        config.repo_path = cwd;
    } else {
        const bool clone = current_repo ? repo_mode == 2 : repo_mode == 0;
        const fs::path fallback = clone ? cwd / "devbox" : cwd;
        config.repo_path = fs::path(prompt_text(clone ? "Clone destination" : "Existing checkout path", fallback.string()));
    }

    const bool docker_ok = has_tool(tools, "Docker");
    std::size_t recommended_runtime = 1;
#ifdef _WIN32
    recommended_runtime = docker_ok ? 0 : 1;
#else
    recommended_runtime = 1;
#endif
    const auto runtime = menu(theme, "Runtime", {
        "Auto (Windows: Docker when available/configured; other platforms: host)",
        "Host (direct native execution; simplest cross-platform setup)",
        "Docker (isolated runtime; requires a working Docker engine)"
    }, recommended_runtime);
    config.runtime = runtime == 0 ? RuntimeChoice::Auto : runtime == 1 ? RuntimeChoice::Host : RuntimeChoice::Docker;

    const auto auth = menu(theme, "Authentication", {
        "None (local/trusted network only)",
        "OAuth (built-in connector/test flow; no external identity check)",
        "Cloudflare Access (Cloudflare-backed OAuth; requires Access application details)"
    }, 0);
    config.auth = auth == 0 ? AuthChoice::None : auth == 1 ? AuthChoice::OAuth : AuthChoice::Cloudflare;
    if (config.auth != AuthChoice::None) {
        config.public_base_url = prompt_required_text("Public MCP base URL (for example https://mcp.example.com)");
    }
    if (config.auth == AuthChoice::Cloudflare) {
        config.cloudflare_team_domain = prompt_required_text("Cloudflare Access team domain (https://<team>.cloudflareaccess.com)");
        config.cloudflare_aud = prompt_required_text("Cloudflare Access application audience (AUD)");
        std::string default_jwks = config.cloudflare_team_domain;
        while (!default_jwks.empty() && default_jwks.back() == '/') default_jwks.pop_back();
        default_jwks += "/cdn-cgi/access/certs";
        config.cloudflare_jwks_url = prompt_text("Cloudflare Access JWKS URL", default_jwks);
    }

    if (config.auth != AuthChoice::None) {
        std::cout << "\n" << theme.paint(theme.bold, "Public transport check") << "\n";
        if (!has_tool(tools, "cloudflared")) {
            std::cout << theme.paint(theme.yellow, "cloudflared is not installed.") << "\n"
                      << "Authentication can still work behind another HTTPS reverse proxy.\n"
                      << "For Cloudflare Tunnel transport on this machine, install it with:\n"
                      << cloudflared_install_hint(platform) << "\n"
                      << "Then run `devbox-tui --cloudflare-help` or read docs/CLOUDFLARE_TUNNEL.md.\n\n";
        } else {
            std::cout << theme.paint(theme.green, "cloudflared is available.") << "\n";
#ifndef _WIN32
            std::cout << "After setup, add the tunnel token/hostname to .env and run:\n"
                      << (platform.termux ? "  sh scripts/install-cloudflare-tunnel.sh termux\n" : "  sh scripts/install-cloudflare-tunnel.sh auto\n")
                      << "See docs/CLOUDFLARE_TUNNEL.md for the dashboard and troubleshooting steps.\n\n";
#else
            std::cout << "Windows host mode can start the token-based named tunnel after the token/hostname are present in .env.\n\n";
#endif
        }
    }

    config.host = prompt_text("Bind address", "127.0.0.1");
    config.port = prompt_port(8100);
    const std::string workspace = prompt_text("Workspace (blank uses <repo>/workspace)", "");
    if (!workspace.empty()) config.workspace = fs::path(workspace);

    std::cout << "\n" << theme.paint(theme.bold, "Installation options") << "\n";
    const bool required_missing = std::any_of(tools.begin(), tools.end(), [](const ToolStatus& item) { return item.required && !item.available; });
    config.install_system_packages = prompt_yes_no("Install missing Node.js/Git prerequisites using the platform package manager", required_missing);
    config.install_dependencies = prompt_yes_no("Run npm install", true);
    config.link_command = prompt_yes_no("Install/link the global devbox command", true);
    config.start_server = prompt_yes_no("Start Devbox and verify /healthz", true);
    config.install_guardian = prompt_yes_no("Install Guardian reliability supervision", true);

    std::cout << "\n";
    rule(theme);
    print_plan(theme, config);
    rule(theme);
    if (!prompt_yes_no("Apply this plan", true)) {
        std::cout << "Cancelled without changing the system.\n";
        return 0;
    }

    const auto args = build_bootstrap_args(*bootstrap, config);
    std::cout << "\n" << theme.paint(theme.cyan, theme.paint(theme.bold, "Running setup")) << "\n";
    rule(theme);
    const int code = run_process(args);
    rule(theme);
    if (code != 0) {
        std::cerr << theme.paint(theme.red, "Setup failed with exit code " + std::to_string(code)) << "\n";
        std::cerr << "Review the error above, fix the reported prerequisite, and rerun this TUI.\n";
        return code;
    }

    std::cout << theme.paint(theme.green, theme.paint(theme.bold, "Setup completed successfully.")) << "\n";
    if (!config.dry_run) {
        std::cout << "Next commands:\n"
                  << "  devbox status\n"
                  << "  devbox restart\n"
                  << "  devbox stop\n";
        std::cout << "MCP endpoint: http://" << (config.host == "0.0.0.0" ? "127.0.0.1" : config.host)
                  << ':' << config.port << "/mcp\n";
    }
    return 0;
}

void print_help() {
    std::cout << "Devbox Setup TUI " << kVersion << "\n\n"
              << "Usage: devbox-tui [options]\n\n"
              << "  --bootstrap <path>  Explicit devbox-setup binary\n"
              << "  --diagnostics       Print platform/tool diagnostics and exit\n"
              << "  --cloudflare-help   Print platform-specific Cloudflare Tunnel setup instructions\n"
              << "  --dry-run           Guide through setup but make the Rust bootstrap print its plan only\n"
              << "  --no-color          Disable ANSI color\n"
              << "  -h, --help          Show this help\n"
              << "  -V, --version       Show version\n";
}

}  // namespace

int main(int argc, char** argv) {
    enable_virtual_terminal();
    Theme theme;
    theme.color = stdout_is_tty() && getenv_string("NO_COLOR").empty();
    const PlatformInfo platform = detect_platform();
    std::optional<std::string> bootstrap;
    bool diagnostics = false;
    bool cloudflare_help = false;
    bool dry_run = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bootstrap") {
            if (i + 1 >= argc) {
                std::cerr << "--bootstrap requires a path\n";
                return 2;
            }
            bootstrap = argv[++i];
        } else if (arg == "--diagnostics") {
            diagnostics = true;
        } else if (arg == "--cloudflare-help") {
            cloudflare_help = true;
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--no-color") {
            theme.color = false;
        } else if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-V" || arg == "--version") {
            std::cout << "devbox-tui " << kVersion << "\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_help();
            return 2;
        }
    }

    if (diagnostics) {
        print_diagnostics(theme, platform);
        return 0;
    }
    if (cloudflare_help) {
        print_cloudflare_help(theme, platform);
        return 0;
    }
    if (!stdin_is_tty()) {
        std::cerr << "devbox-tui needs an interactive terminal. Use devbox-setup directly for scripts/CI.\n";
        return 2;
    }
    return interactive_setup(theme, platform, bootstrap, dry_run);
}
