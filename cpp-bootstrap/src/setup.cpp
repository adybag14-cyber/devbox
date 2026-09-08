#include "devbox/setup.hpp"
#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include "devbox/runtime.hpp"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <set>
#include <thread>
namespace devbox::setup {
namespace {
constexpr auto vcpkg_revision = "04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4";
std::uint64_t number(std::string_view text, const char* name) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
        throw Error(std::string("invalid ") + name + ": " + std::string(text));
    return value;
}
std::string choice(std::string text, std::initializer_list<const char*> choices, const char* name) {
    text = lower(trim(text));
    for (const auto* value : choices)
        if (value == text)
            return text;
    throw Error(std::string("invalid ") + name + ": " + text);
}
std::string value_text(std::string text) {
    text = trim(text);
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
        return text.substr(1, text.size() - 2);
    for (std::size_t i = 1; i < text.size(); ++i)
        if (text[i] == '#' && std::isspace(static_cast<unsigned char>(text[i - 1])))
            return trim(text.substr(0, i));
    return text;
}
std::vector<std::string> lines(std::string_view text) {
    auto values = split(text, '\n');
    if (!values.empty() && values.back().empty())
        values.pop_back();
    for (auto& value : values)
        if (value.ends_with('\r'))
            value.pop_back();
    return values;
}
std::string command_name(std::string value) {
#ifdef _WIN32
    if (value == "npm" || value == "npx")
        value += ".cmd";
#endif
    return value;
}
ProcessOutput command(std::string program, std::vector<std::string> args,
                      const std::optional<fs::path>& cwd = {}, bool dry = false,
                      const Environment& extra = {}, bool quiet = false) {
    if (!quiet) {
        std::cout << "> " << program;
        for (const auto& arg : args)
            std::cout << ' ' << (arg.find(' ') == arg.npos ? arg : Json(arg).dump());
        std::cout << '\n' << std::flush;
    }
    if (dry)
        return {};
    ProcessOptions options;
    options.cwd = cwd;
    options.timeout = quiet ? Millis(30000) : Millis(90 * 60 * 1000);
    options.max_capture_chars = quiet ? 65536 : 16000;
    options.env = current_environment();
    for (const auto& [key, value] : extra)
        (*options.env)[key] = value;
    if (!quiet)
        options.on_output = [](OutputStream stream, std::string_view text) {
            auto& output = stream == OutputStream::stdout_stream ? std::cout : std::cerr;
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
        };
    return run_native_program(command_name(program), args, options);
}
std::optional<std::string> capture(std::string program, std::vector<std::string> args) {
    try {
        const auto result = command(std::move(program), std::move(args), {}, false, {}, true);
        return trim(result.stdout_text.empty() ? result.stderr_text : result.stdout_text);
    } catch (const std::exception&) {
        return {};
    }
}
std::string first_command(std::initializer_list<const char*> candidates) {
    for (const auto* name : candidates)
        if (auto path = find_program(name))
            return path_text(*path);
    return {};
}
void refresh_paths() {
    std::vector<fs::path> extras;
#ifdef _WIN32
    const auto programs = path_from_utf8(env_or("ProgramFiles", "C:\\Program Files"));
    extras = {programs / "nodejs", programs / "Git" / "cmd", programs / "CMake" / "bin",
              programs / "LLVM" / "bin"};
    constexpr char separator = ';';
#else
    extras = {"/opt/homebrew/bin", "/usr/local/bin"};
    constexpr char separator = ':';
#endif
    auto paths = split(env_or("PATH", ""), separator, false);
    for (const auto& extra : extras) {
        const auto text = path_text(extra);
        if (fs::is_directory(extra) && std::none_of(paths.begin(), paths.end(), [&](const auto& item) {
                return lower(item) == lower(text);
            }))
            paths.push_back(text);
    }
    set_environment("PATH", join(paths, std::string(1, separator)));
}
void system_command(std::string program, std::vector<std::string> args, const Options& options) {
#ifndef _WIN32
    if (platform_kind() != PlatformKind::termux && geteuid() != 0) {
        const auto sudo = find_program("sudo");
        if (!sudo)
            throw Error("Installing system packages requires root privileges. Install prerequisites manually "
                        "or rerun the installer with sudo.");
        args.insert(args.begin(), program);
        args.insert(args.begin(), "-n");
        program = path_text(*sudo);
    }
#endif
    command(program, args, {}, options.dry_run);
}
void prerequisites(const Options& options) {
    const bool node = !capture("node", {"--version"}), npm = !capture("npm", {"--version"}),
               git = !capture("git", {"--version"});
    if ((!node && !npm && !git) || !options.system_packages)
        return;
    const auto platform = platform_kind();
    if (platform == PlatformKind::windows) {
        const auto winget = first_command({"winget.exe", "winget"});
        if (winget.empty())
            throw Error("Node.js/npm or Git is missing and winget was not found. Install Node.js LTS and "
                        "Git, then rerun devbox-setup.");
        for (const auto& [needed, id] :
             {std::pair{node || npm, "OpenJS.NodeJS.LTS"}, std::pair{git, "Git.Git"}})
            if (needed)
                command(winget,
                        {"install", "--exact", "--id", id, "--accept-package-agreements",
                         "--accept-source-agreements", "--silent"},
                        {}, options.dry_run);
    } else if (platform == PlatformKind::termux) {
        command("pkg",
                {"install", "-y", "nodejs", "git", "python", "ripgrep", "curl", "ca-certificates", "clang",
                 "cmake", "ninja", "pkg-config"},
                {}, options.dry_run);
    } else {
        const auto manager = platform == PlatformKind::macos
                                 ? first_command({"brew", "/opt/homebrew/bin/brew", "/usr/local/bin/brew"})
                                 : first_command({"apt-get", "dnf", "yum", "pacman", "zypper", "apk"});
        if (manager.empty())
            throw Error("Required programs are missing and no supported package manager was found. Install "
                        "Node.js 18+, npm and Git manually.");
        std::vector<std::string> args;
        if (manager.ends_with("pacman"))
            args = {"-S", "--needed", "--noconfirm"};
        else if (manager.ends_with("zypper"))
            args = {"--non-interactive", "install"};
        else if (manager.ends_with("apk"))
            args = {"add"};
        else
            args = platform == PlatformKind::macos ? std::vector<std::string>{"install"}
                                                   : std::vector<std::string>{"install", "-y"};
        if (node || npm) {
            args.push_back(platform == PlatformKind::macos ? "node" : "nodejs");
            if (platform != PlatformKind::macos)
                args.push_back("npm");
        }
        if (git)
            args.push_back("git");
        if (manager.ends_with("apt-get"))
            system_command(manager, {"update"}, options);
        if (platform == PlatformKind::macos)
            command(manager, args, {}, options.dry_run);
        else
            system_command(manager, args, options);
    }
}
void verify_tools() {
    const auto node = capture("node", {"--version"});
    if (!node || node_major(*node).value_or(0) < 18)
        throw Error("Node.js 18+ is required; found " + node.value_or("no Node.js"));
    for (const auto* name : {"npm", "git"}) {
        const auto version = capture(name, {"--version"});
        if (!version)
            throw Error(std::string("Required program is unavailable: ") + name);
        std::cout << name << ": " << *version << '\n';
    }
    std::cout << "Node.js: " << *node << '\n';
}
fs::path locate_repo(const Options& options) {
    const auto current = fs::current_path();
    const auto root = options.repo       ? fs::absolute(*options.repo)
                      : is_repo(current) ? current
                                         : current / "devbox";
    if (is_repo(root)) {
        std::cout << "Using Devbox checkout: " << path_text(root) << '\n';
        return root;
    }
    if (fs::exists(root) && (!fs::is_directory(root) || !fs::is_empty(root)))
        throw Error(path_text(root) + " is not a Devbox checkout and is not empty");
    const auto identity = build_snapshot();
    const auto revision = json_string(identity, "gitSha");
    if (identity.value("sourceDirty", Json()) != false || revision.size() != 40 ||
        !std::all_of(revision.begin(), revision.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw Error("New installations require an installer built from a clean committed source. "
                    "Use a verified release bundle or provide an existing checkout with --repo.");
    if (!options.dry_run)
        fs::create_directories(root.parent_path());
    command("git", {"clone", "--depth", "1", "--", options.repo_url, path_text(root)}, root.parent_path(),
            options.dry_run);
    // The default branch may have advanced since this installer was released.
    // Only a newly created checkout is moved to the binary's source revision.
    command("git", {"fetch", "--depth", "1", "origin", revision}, root, options.dry_run);
    command("git", {"checkout", "--detach", revision}, root, options.dry_run);
    if (!options.dry_run && !is_repo(root))
        throw Error("Clone completed but destination does not look like a C++ Devbox checkout");
    return root;
}
void install_guardian(const fs::path& root, const PreparedConfig& prepared, const Options& options) {
    if (!options.guardian)
        return;
    if (platform_kind() == PlatformKind::windows) {
        const auto shell = first_command({"pwsh.exe", "pwsh", "powershell.exe", "powershell"});
        if (shell.empty())
            throw Error("PowerShell is required to install the Windows Guardian tasks");
        command(shell,
                {"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                 path_text(root / "scripts" / "Install-ChatGptDevboxGuardian.ps1"), "-Runtime",
                 prepared.runtime},
                root, options.dry_run, prepared.environment);
    } else {
        const auto platform = platform_kind();
        command("sh",
                {path_text(root / "scripts" / "install-guardian.sh"),
                 platform == PlatformKind::termux  ? "termux"
                 : platform == PlatformKind::macos ? "launchd"
                                                   : "auto"},
                root, options.dry_run, prepared.environment);
    }
}
} // namespace
Options parse_options(const std::vector<std::string>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& flag = args[i];
        const auto next = [&]() {
            if (++i >= args.size())
                throw Error(flag + " requires a value");
            return args[i];
        };
        if (flag == "--repo")
            options.repo = path_from_utf8(next());
        else if (flag == "--repo-url")
            options.repo_url = next();
        else if (flag == "--workspace")
            options.workspace = path_from_utf8(next());
        else if (flag == "--runtime-binary")
            options.runtime_binary = path_from_utf8(next());
        else if (flag == "--runtime")
            options.runtime = choice(next(), {"auto", "host", "docker"}, "runtime mode");
        else if (flag == "--host")
            options.host = next();
        else if (flag == "--auth")
            options.auth = choice(next(), {"none", "oauth", "cloudflare"}, "auth mode");
        else if (flag == "--public-base-url")
            options.public_url = next();
        else if (flag == "--cloudflare-team-domain")
            options.team_domain = next();
        else if (flag == "--cloudflare-aud")
            options.audience = next();
        else if (flag == "--cloudflare-jwks-url")
            options.jwks_url = next();
        else if (flag == "--port") {
            const auto port = number(next(), "port");
            if (!port || port > 65535)
                throw Error("port must be between 1 and 65535");
            options.port = static_cast<std::uint16_t>(port);
        } else if (flag == "--skip-system-packages")
            options.system_packages = false;
        else if (flag == "--skip-install")
            options.dependencies = false;
        else if (flag == "--no-link")
            options.link = false;
        else if (flag == "--no-start")
            options.start = false;
        else if (flag == "--guardian")
            options.guardian = true;
        else if (flag == "--dry-run")
            options.dry_run = true;
        else if (flag == "--build-runtime-only")
            options.build_only = true;
        else if (flag == "--help" || flag == "-h")
            options.help = true;
        else if (flag == "--version" || flag == "-V")
            options.version = true;
        else
            throw Error("unknown option " + flag + "\n\n" + usage());
    }
    return options;
}
std::string usage() {
    return R"(Devbox C++ MCP bootstrap installer

Usage: devbox-setup [OPTIONS]

Uses an existing checkout or clones ./devbox. Preserves existing configuration
values except explicit options and the C++ implementation selection. Installs
Node.js/npm and Git when required, links the devbox command, then starts MCP.
Bundled native runtimes need no compiler or Rust installation.

  --repo PATH              Existing checkout or clone destination
  --repo-url URL           Repository to clone
  --runtime auto|host|docker
  --host ADDRESS          Set HOST
  --port PORT             Set PORT (1-65535)
  --workspace PATH        Host workspace and working directory
  --auth none|oauth|cloudflare
  --public-base-url URL    Required for OAuth/Cloudflare
  --cloudflare-team-domain URL
  --cloudflare-aud AUD
  --cloudflare-jwks-url URL
  --runtime-binary PATH   Use a verified local C++ runtime candidate
  --build-runtime-only    Build and stage the native runtime, without setup
  --skip-system-packages  Do not install system prerequisites
  --skip-install          Do not install npm dependencies
  --no-link               Do not link the global command
  --no-start              Configure without starting the server
  --guardian              Install Guardian supervision
  --dry-run               Print actions without changing files or services
  -h, --help              Show help
  -V, --version           Show version
)";
}
std::optional<std::string> env_key(std::string_view line) {
    auto value = trim(line);
    if (value.empty() || value.front() == '#')
        return {};
    if (starts_with(value, "export "))
        value = value.substr(7);
    const auto equal = value.find('=');
    return equal == value.npos ? std::nullopt : std::optional(trim(value.substr(0, equal)));
}
std::string set_env_value(std::string content, std::string_view key, std::string_view value) {
    if (value.find_first_of("\r\n") != value.npos)
        throw Error("Configuration values must not contain newlines: " + std::string(key));
    auto values = lines(content);
    bool found = false;
    for (auto& line : values)
        if (!found && env_key(line) == key) {
            line = std::string(key) + "=" + std::string(value);
            found = true;
        }
    if (!found) {
        if (!values.empty() && !values.back().empty())
            values.emplace_back();
        values.emplace_back(std::string(key) + "=" + std::string(value));
    }
    return join(values, "\n") + '\n';
}
std::optional<std::string> get_env_value(std::string_view content, std::string_view key) {
    for (const auto& line : lines(content))
        if (env_key(line) == key)
            return value_text(line.substr(line.find('=') + 1));
    return {};
}
Environment collect_env_values(std::string_view content) {
    Environment result;
    for (const auto& line : lines(content))
        if (const auto key = env_key(line))
            result[*key] = value_text(line.substr(line.find('=') + 1));
    return result;
}
bool is_termux(std::optional<std::string> version, std::optional<std::string> prefix) {
    return (version && !trim(*version).empty()) ||
           (prefix && prefix->find("com.termux/files/usr") != prefix->npos);
}
PlatformKind platform_kind() {
    if (is_termux(environment("TERMUX_VERSION"), environment("PREFIX")))
        return PlatformKind::termux;
#ifdef _WIN32
    return PlatformKind::windows;
#elif defined(__APPLE__)
    return PlatformKind::macos;
#elif defined(__linux__) || defined(__ANDROID__)
    return PlatformKind::linux;
#else
    return PlatformKind::other;
#endif
}
std::string platform_name(PlatformKind value) {
    switch (value) {
    case PlatformKind::windows:
        return "Windows";
    case PlatformKind::macos:
        return "macOS";
    case PlatformKind::linux:
        return "Linux";
    case PlatformKind::termux:
        return "Termux / Android";
    default:
        return "Other";
    }
}
std::string recommended_runtime(PlatformKind platform, bool docker_available) {
    return (platform == PlatformKind::windows && docker_available) || platform == PlatformKind::other
               ? "auto"
               : "host";
}
std::optional<unsigned> node_major(std::string version) {
    version = trim(version);
    if (starts_with(version, "v"))
        version.erase(0, 1);
    const auto dot = version.find('.');
    try {
        const auto value = number(version.substr(0, dot), "Node.js version");
        return value <= UINT_MAX ? std::optional(static_cast<unsigned>(value)) : std::nullopt;
    } catch (...) {
        return {};
    }
}
bool is_repo(const fs::path& root) {
    return fs::is_regular_file(root / "package.json") && fs::is_regular_file(root / ".env.example") &&
           fs::is_regular_file(root / "src" / "server.js") &&
           fs::is_regular_file(root / "cpp-mcp" / "CMakeLists.txt");
}
PreparedConfig prepare_files(const fs::path& root, const Options& options) {
    const auto path = root / ".env";
    const bool created = !fs::exists(path);
    auto content = read_file(created ? root / ".env.example" : path, 4 * 1024 * 1024);
    if (starts_with(content, "\xef\xbb\xbf"))
        content.erase(0, 3);
    const auto set = [&](const char* key, const std::string& value) {
        content = set_env_value(std::move(content), key, value);
    };
    set("DEVBOX_MCP_IMPLEMENTATION", "cpp");
    if (created || options.runtime)
        set("DEVBOX_RUNTIME_MODE",
            options.runtime
                ? *options.runtime
                : recommended_runtime(
                      platform_kind(),
                      capture("docker", {"version", "--format", "{{.Server.Version}}"}).has_value()));
    if (options.host)
        set("HOST", *options.host);
    if (options.port)
        set("PORT", std::to_string(*options.port));
    if (options.auth)
        set("MCP_AUTH_MODE", *options.auth == "oauth"        ? "demo-oauth"
                             : *options.auth == "cloudflare" ? "cloudflare-access"
                                                             : "none");
    for (const auto& [key, value] : {std::pair{"PUBLIC_BASE_URL", options.public_url},
                                     std::pair{"CLOUDFLARE_ACCESS_TEAM_DOMAIN", options.team_domain},
                                     std::pair{"CLOUDFLARE_ACCESS_AUD", options.audience},
                                     std::pair{"CLOUDFLARE_ACCESS_JWKS_URL", options.jwks_url}})
        if (value)
            set(key, trim(*value));
    if (options.auth && *options.auth != "none" &&
        trim(get_env_value(content, "PUBLIC_BASE_URL").value_or("")).empty())
        throw Error("--auth " + *options.auth +
                    " requires --public-base-url (or an existing PUBLIC_BASE_URL in .env)");
    if (options.auth == "cloudflare") {
        auto team = trim(get_env_value(content, "CLOUDFLARE_ACCESS_TEAM_DOMAIN").value_or(""));
        if (team.empty() || trim(get_env_value(content, "CLOUDFLARE_ACCESS_AUD").value_or("")).empty())
            throw Error("--auth cloudflare requires Cloudflare team domain and audience");
        if (trim(get_env_value(content, "CLOUDFLARE_ACCESS_JWKS_URL").value_or("")).empty()) {
            while (team.ends_with('/'))
                team.pop_back();
            set("CLOUDFLARE_ACCESS_JWKS_URL", team + "/cdn-cgi/access/certs");
        }
    }
    const auto workspace =
        options.workspace ? options.workspace->is_absolute() ? *options.workspace : root / *options.workspace
                          : root / "workspace";
    if (options.workspace || (platform_kind() == PlatformKind::termux && created)) {
        set("HOST_WORKSPACE_PATH", path_text(workspace));
        set("HOST_DEFAULT_WORKDIR", path_text(workspace));
    }
    if (platform_kind() == PlatformKind::termux && created) {
        const auto bash = path_from_utf8(env_or("PREFIX", "")) / "bin" / "bash";
        if (fs::is_regular_file(bash))
            set("HOST_SHELL", path_text(bash));
        else if (const auto shell = environment("SHELL"))
            set("HOST_SHELL", *shell);
    }
    if (options.dry_run)
        std::cout << "Would write configuration: " << path_text(path)
                  << "\nWould create: " << path_text(workspace)
                  << "\nWould create: " << path_text(root / "run") << '\n';
    else {
        write_file(path, content);
        fs::create_directories(workspace);
        fs::create_directories(root / "run");
    }
    PreparedConfig prepared;
    prepared.host = get_env_value(content, "HOST").value_or("0.0.0.0");
    if (prepared.host.empty())
        prepared.host = "0.0.0.0";
    try {
        const auto port = number(get_env_value(content, "PORT").value_or("8100"), "port");
        if (port > 0 && port <= 65535)
            prepared.port = static_cast<std::uint16_t>(port);
    } catch (...) {
    }
    prepared.runtime = get_env_value(content, "DEVBOX_RUNTIME_MODE").value_or("auto");
    prepared.content = content;
    prepared.environment = collect_env_values(content);
    return prepared;
}
std::string loopback_host(std::string host) {
    return host == "::" || host == "[::]" ? "::1" : host.empty() || host == "0.0.0.0" ? "127.0.0.1" : host;
}
std::string host_port(std::string host, unsigned port) {
    if (host.find(':') != host.npos && !host.starts_with('['))
        host = '[' + host + ']';
    return host + ':' + std::to_string(port);
}
bool health_check(const std::string& host, unsigned port) {
    try {
        const auto result = http_request("GET", "http://" + host_port(loopback_host(host), port) + "/healthz",
                                         {}, Json::object(), Millis(1500), 64);
        return result.status == 200 && trim(result.body) == "ok";
    } catch (...) {
        return false;
    }
}
std::string cloudflare_hint(PlatformKind platform, std::string manager) {
    if (platform == PlatformKind::windows)
        return "winget install --id Cloudflare.cloudflared --exact";
    if (platform == PlatformKind::macos)
        return "brew install cloudflared";
    if (platform == PlatformKind::termux)
        return "pkg update && pkg install cloudflared termux-services";
    if (manager.ends_with("pacman"))
        return "sudo pacman -Syu cloudflared";
    if (manager.ends_with("apt-get"))
        return "Use the Cloudflare signed repository (https://pkg.cloudflare.com/cloudflare-main.gpg); see "
               "docs/CLOUDFLARE_TUNNEL.md.";
    if (manager.ends_with("dnf") || manager.ends_with("yum"))
        return "Use https://pkg.cloudflare.com/cloudflared.repo; see docs/CLOUDFLARE_TUNNEL.md.";
    return "Install the official cloudflared package/binary for this platform; see "
           "docs/CLOUDFLARE_TUNNEL.md.";
}
fs::path build_runtime(const fs::path& root, const Options& options) {
#ifdef _WIN32
    const auto basename = "devbox-mcp.exe";
#else
    const auto basename = "devbox-mcp";
#endif
    std::optional<fs::path> candidate = options.runtime_binary;
    if (!candidate) {
        const auto executable = executable_path();
        std::vector<fs::path> candidates{executable.parent_path() / basename};
        const auto self_name = path_text(executable.filename());
        const std::string prefix = "devbox-setup-";
        if (starts_with(self_name, prefix))
            candidates.push_back(executable.parent_path() /
                                 path_from_utf8("devbox-mcp-" + self_name.substr(prefix.size())));
        candidates.push_back(root / "bin" / "native" / basename);
        for (const auto& path : candidates)
            if (fs::is_regular_file(path)) {
                candidate = path;
                break;
            }
    }
    if (!candidate) {
        const auto cmake = first_command({"cmake"});
        if (cmake.empty())
            throw Error("Building the C++ runtime requires CMake 3.24+ and a C++20 compiler, or a bundled "
                        "devbox-mcp binary supplied with --runtime-binary.");
        auto vcpkg = path_from_utf8(env_or("VCPKG_ROOT", path_text(root / ".cpp-build" / "vcpkg")));
        if (!fs::is_regular_file(vcpkg / "scripts" / "buildsystems" / "vcpkg.cmake")) {
            command(
                "git",
                {"clone", "--filter=blob:none", "https://github.com/microsoft/vcpkg.git", path_text(vcpkg)},
                root, options.dry_run);
            command("git", {"checkout", "--detach", vcpkg_revision}, vcpkg, options.dry_run);
#ifdef _WIN32
            command(path_text(vcpkg / "bootstrap-vcpkg.bat"), {"-disableMetrics"}, vcpkg, options.dry_run);
#else
            command("sh", {path_text(vcpkg / "bootstrap-vcpkg.sh"), "-disableMetrics"}, vcpkg,
                    options.dry_run);
#endif
        }
        const auto build = root / ".cpp-build" / "managed";
        std::vector<std::string> args{
            "-S",
            path_text(root),
            "-B",
            path_text(build),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DDEVBOX_BUILD_TESTS=OFF",
            "-DDEVBOX_BUILD_TUI=OFF",
            "-DCMAKE_TOOLCHAIN_FILE=" + path_text(vcpkg / "scripts" / "buildsystems" / "vcpkg.cmake"),
            "-DVCPKG_INSTALLED_DIR=" + path_text(root / ".cpp-build" / "vcpkg_installed")};
#ifdef _WIN32
        args.push_back("-DVCPKG_TARGET_TRIPLET=x64-windows-static");
#endif
        command(cmake, args, root, options.dry_run, {{"VCPKG_MAX_CONCURRENCY", "4"}});
        command(
            cmake,
            {"--build", path_text(build), "--config", "Release", "--target", "devbox-mcp", "--parallel", "4"},
            root, options.dry_run);
        candidate = build / "cpp-mcp" / basename;
        if (!options.dry_run && !fs::is_regular_file(*candidate))
            candidate = build / "cpp-mcp" / "Release" / basename;
    }
    if (options.dry_run) {
        std::cout << "Would verify and stage C++ runtime: " << path_text(*candidate) << '\n';
        return *candidate;
    }
    const auto info =
        Json::parse(command(path_text(*candidate), {"--build-info"}, root, false, {}, true).stdout_text);
    if (json_string(info, "implementation") != "cpp")
        throw Error("Runtime candidate is not a C++ Devbox executable");
    const auto hash = sha256_file(*candidate);
    if (json_string(info, "binarySha256") != hash)
        throw Error("C++ runtime self-reported hash does not match the file");
    const auto destination = root / "bin" / "native" / basename;
    fs::create_directories(destination.parent_path());
    if (fs::absolute(*candidate) != fs::absolute(destination)) {
        const auto staging = destination.parent_path() / path_from_utf8(".mcp-install-" + uuid());
        ScopeExit cleanup([&] {
            std::error_code ec;
            fs::remove(staging, ec);
        });
        fs::copy_file(*candidate, staging, fs::copy_options::overwrite_existing);
        if (sha256_file(staging) != hash)
            throw Error("C++ runtime staging hash mismatch");
        replace_state_file(staging, destination);
    }
    return destination;
}
void run(const Options& options) {
    std::cout << "Devbox C++ MCP setup " << installer_version
              << "\nPlatform: " << platform_name(platform_kind()) << '\n';
    if (options.build_only) {
        const auto root = locate_repo(options);
        std::cout << path_text(build_runtime(root, options)) << '\n';
        return;
    }
    refresh_paths();
    prerequisites(options);
    refresh_paths();
    if (!options.dry_run)
        verify_tools();
    const auto root = locate_repo(options);
    if (options.dry_run && !is_repo(root)) {
        std::cout << "Dry run complete; configuration follows cloning.\n";
        return;
    }
    const auto prepared = prepare_files(root, options);
    if (prepared.runtime == "docker" && !capture("docker", {"--version"}))
        std::cerr << "Docker mode is configured but Docker is unavailable. Install Docker or choose "
                     "--runtime host.\n";
    if (options.auth && *options.auth != "none" && !capture("cloudflared", {"--version"}))
        std::cout << "Optional Cloudflare Tunnel transport: "
                  << cloudflare_hint(platform_kind(),
                                     first_command({"apt-get", "dnf", "yum", "pacman", "apk"}))
                  << '\n';
    if (options.dependencies)
        command("npm", {fs::is_regular_file(root / "package-lock.json") ? "ci" : "install"}, root,
                options.dry_run);
    if (options.link) {
        try {
            command("npm", {"link"}, root, options.dry_run);
        } catch (const std::exception& e) {
            std::cerr << "npm link failed: " << e.what()
                      << "\nUse node bin/devbox.js or rerun npm link later.\n";
        }
    }
    // A --no-start installation must still be ready for a later launcher start.
    (void)build_runtime(root, options);
    if (options.start) {
        command("node", {"bin/devbox.js", "start"}, root, options.dry_run, prepared.environment);
        if (!options.dry_run) {
            const auto deadline = Clock::now() + Millis(30000);
            bool healthy = false;
            do {
                healthy = health_check(prepared.host, prepared.port);
                if (healthy)
                    break;
                std::this_thread::sleep_for(Millis(250));
            } while (Clock::now() < deadline);
            if (!healthy)
                throw Error("Devbox started but health check failed; inspect " +
                            path_text(root / "run" / "devbox.log"));
        }
    }
    try {
        install_guardian(root, prepared, options);
    } catch (const std::exception& e) {
        std::cerr << "Devbox is installed, but Guardian setup failed: " << e.what()
                  << "\nRerun the Guardian installation script after correcting the error.\n";
    }
    const auto address = host_port(loopback_host(prepared.host), prepared.port);
    std::cout << "\nSetup complete.\nRepository: " << path_text(root) << "\nRuntime: " << prepared.runtime
              << "\nMCP URL: http://" << address << "/mcp\nHealth URL: http://" << address
              << "/healthz\nCommands: devbox status | devbox restart | devbox stop | devbox run\n";
}
} // namespace devbox::setup
