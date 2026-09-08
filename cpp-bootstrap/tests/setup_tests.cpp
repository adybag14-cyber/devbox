#include "devbox/native.hpp"
#include "devbox/setup.hpp"
#include <iostream>
using namespace devbox;
using namespace devbox::setup;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw Error(message);
}
template <class Fn> void rejects(Fn action, std::string_view message) {
    try {
        action();
    } catch (const std::exception& e) {
        require(std::string(e.what()).find(message) != std::string::npos, e.what());
        return;
    }
    throw Error("Expected rejection: " + std::string(message));
}
} // namespace
int main(int argc, char** argv) {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-setup-test-" + uuid());
    fs::create_directories(root / "cpp-mcp");
    fs::create_directories(root / "src");
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        require(node_major("v24.1.0") == 24 && node_major("18.20.0") == 18 && !node_major("unknown"),
                "Node.js version validation");
        require(is_termux("0.118", {}) && is_termux({}, "/data/data/com.termux/files/usr") &&
                    !is_termux("", {}) && !is_termux({}, "/usr"),
                "Termux environment detection");
        require(recommended_runtime(PlatformKind::windows, true) == "auto" &&
                    recommended_runtime(PlatformKind::windows, false) == "host" &&
                    recommended_runtime(PlatformKind::linux, true) == "host",
                "platform runtime recommendations");
        require(loopback_host("::") == "::1" && host_port("::1", 8100) == "[::1]:8100" &&
                    host_port("[::1]", 8100) == "[::1]:8100",
                "IPv6 endpoint formatting");
        rejects([] { (void)parse_options({"--port", "0"}); }, "port must be");
        rejects([] { (void)parse_options({"--port", "65536"}); }, "port must be");
        rejects([] { (void)parse_options({"--auth", "invalid"}); }, "invalid auth mode");
        rejects([] { (void)parse_options({"--runtime"}); }, "requires a value");
        rejects([] { (void)parse_options({"--unknown"}); }, "unknown option");
        auto parsed =
            parse_options({"--repo", path_text(root), "--runtime", "host", "--port", "9123", "--auth",
                           "oauth", "--public-base-url", "https://mcp.example.test", "--skip-system-packages",
                           "--skip-install", "--no-link", "--no-start", "--guardian"});
        require(parsed.port == 9123 && parsed.auth == "oauth" && !parsed.system_packages &&
                    !parsed.dependencies && !parsed.link && !parsed.start && parsed.guardian,
                "all installer options retained");
        auto content = set_env_value("# retained comment\nexport PORT=8100\nCUSTOM=untouched # annotation\n",
                                     "PORT", "9123");
        require(get_env_value(content, "PORT") == "9123" && get_env_value(content, "CUSTOM") == "untouched" &&
                    content.find("# retained comment") != content.npos,
                "config comments and exports preserved");
        const auto environment =
            collect_env_values("A=\"two words\"\nB='literal # hash'\nEMPTY=\nINLINE=value # comment\n");
        require(environment.at("A") == "two words" && environment.at("B") == "literal # hash" &&
                    environment.at("EMPTY").empty() && environment.at("INLINE") == "value",
                "child environment values");
        rejects([&] { (void)set_env_value(content, "HOST", "one\nINJECTED=1"); },
                "must not contain newlines");
        for (const auto& path :
             {root / "package.json", root / "src" / "server.js", root / "cpp-mcp" / "CMakeLists.txt"})
            write_file(path, "{}");
        write_file(root / ".env.example", "\xef\xbb\xbf# fixture\nHOST=127.0.0.1\nPORT=8123\nCUSTOM=keep "
                                          "this\nMCP_AUTH_MODE=none\nPUBLIC_BASE_URL=\n");
        require(is_repo(root), "C++ repository authority");
        auto options = parsed;
        options.guardian = false;
        options.dry_run = true;
        options.workspace = path_from_utf8("workspace with spaces");
        const auto dry = prepare_files(root, options);
        require(!fs::exists(root / ".env") && !fs::exists(root / "run") &&
                    !fs::exists(root / "workspace with spaces"),
                "dry run leaves every directory unchanged");
        require(dry.environment.at("DEVBOX_MCP_IMPLEMENTATION") == "cpp" &&
                    dry.environment.at("MCP_AUTH_MODE") == "demo-oauth",
                "C++ and OAuth configuration");
        options.dry_run = false;
        const auto actual = prepare_files(root, options);
        require(actual.content == dry.content && read_file(root / ".env") == actual.content,
                "dry-run plan equals applied configuration");
        require(fs::is_directory(root / "workspace with spaces") &&
                    actual.environment.at("HOST_WORKSPACE_PATH") == path_text(root / "workspace with spaces"),
                "workspace path retained exactly");
        Options update;
        update.start = false;
        update.port = 9012;
        const auto updated = prepare_files(root, update);
        require(updated.port == 9012 && updated.runtime == "host" &&
                    updated.environment.at("CUSTOM") == "keep this" &&
                    updated.environment.at("MCP_AUTH_MODE") == "demo-oauth",
                "existing choices preserved without explicit overrides");
        update.auth = "cloudflare";
        update.public_url = "https://mcp.example.test";
        update.team_domain = "https://team.cloudflareaccess.com/";
        update.audience = "test-audience";
        const auto cloudflare = prepare_files(root, update);
        require(cloudflare.environment.at("CLOUDFLARE_ACCESS_JWKS_URL") ==
                    "https://team.cloudflareaccess.com/cdn-cgi/access/certs",
                "Cloudflare discovery URL derived");
        const auto before = read_file(root / ".env");
        update.public_url = "";
        rejects([&] { (void)prepare_files(root, update); }, "requires --public-base-url");
        require(read_file(root / ".env") == before, "invalid setup does not alter saved configuration");
        if (argc == 2) {
            Options binary;
            binary.runtime_binary = path_from_utf8(argv[1]);
            const auto staged = build_runtime(root, binary);
            require(staged.parent_path() == root / "bin" / "native" &&
                        sha256_file(staged) == sha256_file(*binary.runtime_binary),
                    "native runtime staged with verified integrity");
            (void)build_runtime(root, binary);
            require(sha256_file(staged) == sha256_file(*binary.runtime_binary),
                    "runtime restaging is byte exact");
        }
        std::cout << "setup tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
