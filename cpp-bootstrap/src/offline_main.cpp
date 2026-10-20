#include "devbox/bundle.hpp"
#include "devbox/management.hpp"
#include "devbox/native.hpp"
#include "offline_manifest.hpp"
#include <charconv>
#include <iostream>

namespace {
using namespace devbox;
fs::path user_root() {
#ifdef _WIN32
    const auto base = environment("LOCALAPPDATA");
    if (!base || base->empty())
        throw Error("OFFLINE_LOCALAPPDATA_REQUIRED: pass --portable PATH");
    return path_from_utf8(*base) / "Programs" / "Devbox";
#elif defined(__APPLE__)
    const auto base = environment("HOME");
    if (!base || base->empty())
        throw Error("OFFLINE_HOME_REQUIRED: pass --portable PATH");
    return path_from_utf8(*base) / "Library" / "Application Support" / "Devbox";
#else
    if (const auto data = environment("XDG_DATA_HOME"); data && !data->empty()) {
        const auto path = path_from_utf8(*data);
        if (!path.is_absolute())
            throw Error("OFFLINE_XDG_DATA_HOME_MUST_BE_ABSOLUTE");
        return path / "devbox";
    }
    const auto base = environment("HOME");
    if (!base || base->empty())
        throw Error("OFFLINE_HOME_REQUIRED: pass --portable PATH");
    return path_from_utf8(*base) / ".local" / "share" / "devbox";
#endif
}
int run(const std::vector<std::string>& args) {
    try {
        std::optional<fs::path> selected;
        bool start = true, inspect = false;
        unsigned port = 8100;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (arg == "--help" || arg == "-h") {
                std::cout
                    << "Devbox C++23 offline installer\n"
                       "  --user                Install under the current user's data directory (default)\n"
                       "  --portable PATH       Install in a new empty directory you own\n"
                       "  --no-start            Install without launching the local server\n"
                       "  --port NUMBER         Local server port (default 8100)\n"
                       "  --inspect             Verify the bundled payload and print its manifest\n"
                       "No administrator rights, package manager, download or developer tools required.\n"
                       "The initial server binds to 127.0.0.1. No system service is registered.\n";
                return 0;
            }
            if (arg == "--user") {
                if (selected)
                    throw Error("OFFLINE_DESTINATION_SPECIFIED_TWICE");
                selected = user_root();
            } else if (arg == "--portable" || arg == "--root") {
                if (selected || ++i == args.size())
                    throw Error("OFFLINE_DESTINATION_REQUIRED_ONCE");
                selected = fs::absolute(path_from_utf8(args[i]));
            } else if (arg == "--port") {
                if (++i == args.size())
                    throw Error("OFFLINE_PORT_REQUIRED");
                const auto& value = args[i];
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !port ||
                    port > 65535)
                    throw Error("OFFLINE_PORT_INVALID");
            } else if (arg == "--no-start") {
                start = false;
            } else if (arg == "--inspect") {
                inspect = true;
            } else {
                throw Error("Unknown offline installer argument: " + arg);
            }
        }
        const auto manifest = Json::parse(embedded_offline_manifest,
                                          embedded_offline_manifest + sizeof(embedded_offline_manifest) - 1);
        bundle::validate(manifest);
        const auto self = fs::canonical(executable_path());
#ifdef __APPLE__
        const auto payload = self.parent_path().parent_path() / "Resources" / "devbox.payload";
        const std::uint64_t offset = 0;
        if (fs::file_size(payload) != json_uint(manifest, "payloadBytes"))
            throw Error("OFFLINE_PAYLOAD_SIZE_MISMATCH");
#else
        const auto& payload = self;
        const auto offset = bundle::payload_offset(payload, json_uint(manifest, "payloadBytes"));
#endif
        if (inspect) {
            if (bundle::region_hash(payload, offset, json_uint(manifest, "payloadBytes")) !=
                json_string(manifest, "payloadSha256"))
                throw Error("OFFLINE_PAYLOAD_DIGEST_MISMATCH");
            std::cout << manifest.dump(2) << '\n';
            return 0;
        }
        const auto root = fs::weakly_canonical(selected ? *selected : user_root());
        if (!root.is_absolute() || root == root.root_path() || (fs::exists(root) && !fs::is_empty(root)))
            throw Error("OFFLINE_NEW_USER_DIRECTORY_REQUIRED");
        ensure_directory(root.parent_path());
        const auto scratch = root.parent_path() / (".devbox-payload-" + uuid());
        bundle::extract(payload, offset, manifest, scratch);
        ScopeExit cleanup([&] {
            std::error_code ignored;
            fs::remove_all(scratch, ignored);
        });
#ifdef _WIN32
        const auto runtime = "devbox-mcp.exe";
#else
        const auto runtime = "devbox-mcp";
#endif
        BundledQualification proof;
        proof.source = json_string(manifest, "source");
        proof.tree = json_string(manifest, "tree");
        proof.target = json_string(manifest, "target");
        proof.workflow_run = json_string(manifest, "workflowRunId");
        proof.contents = scratch;
        unsigned runtimes = 0;
        for (const auto& entry : manifest.at("files"))
            if (json_string(entry, "name") == runtime && json_bool(entry, "executable")) {
                ++runtimes;
                proof.sha256 = json_string(entry, "sha256");
                proof.bytes = json_uint(entry, "bytes");
            }
        if (runtimes != 1)
            throw Error("OFFLINE_EXACT_RUNTIME_REQUIRED");
        management_main({"init", "--root", path_text(root), "--binary", path_text(scratch / runtime),
                         "--port", std::to_string(port)},
                        &proof);
        if (start) {
            ProcessOptions process;
            process.cwd = root;
            process.timeout = Millis(45000);
            process.max_capture_chars = 65536;
            const auto result = spawn_process(path_text(root / "bin" / runtime),
                                              {"manage", "start", "--root", path_text(root)}, process);
            if (result.exit_code)
                throw Error("OFFLINE_INSTALLED_START_FAILED: " + result.stderr_text.substr(0, 2000));
            std::cout << result.stdout_text;
        }
        std::cout << "Installed to " << path_text(root)
                  << "\nNative CLI: " << path_text(root / "bin" / runtime)
                  << " manage status|start|stop --root " << path_text(root) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Offline setup failed: " << error.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(devbox::narrow(argv[i]));
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
