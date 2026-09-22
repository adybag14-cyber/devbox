#include "devbox/grants.hpp"
#include "devbox/isolation.hpp"
#include "devbox/state_store.hpp"
#include <boost/asio.hpp>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <aclapi.h>
#include <processthreadsapi.h>
#include <sddl.h>
#endif
using namespace devbox;
namespace asio = boost::asio;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
int run(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-isolation-" + uuid());
    try {
        ensure_private_state_directory(root);
        const auto workspace = root / "workspace";
        ensure_private_state_directory(workspace);
#ifdef _WIN32
        const auto all_apps = workspace / "all-apps-only.txt";
        write_file(all_apps, "SYNTHETIC-ALL-APPS-CREDENTIAL");
        HANDLE raw_token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token), "fixture token");
        NativeHandle token(raw_token);
        DWORD token_bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_bytes);
        std::vector<unsigned char> token_user(token_bytes);
        require(GetTokenInformation(token.get(), TokenUser, token_user.data(), token_bytes, &token_bytes),
                "fixture user");
        LPWSTR user_sid = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(token_user.data())->User.Sid, &user_sid),
                "fixture SID");
        ScopeExit free_user([&] { LocalFree(user_sid); });
        const auto sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + std::wstring(user_sid) + L")(A;;FR;;;S-1-15-2-1)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                     &descriptor, nullptr),
                "fixture ACL");
        ScopeExit free_descriptor([&] { LocalFree(descriptor); });
        require(SetFileSecurityW(all_apps.c_str(),
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor),
                "protected all-apps-only fixture");
#endif
        write_file(root / "outside-secret", "SYNTHETIC-OUTSIDE-CREDENTIAL");
        set_environment("OPENAI_API_KEY", "SYNTHETIC-AMBIENT-CREDENTIAL");
        set_environment("ISOLATION_TEST_SECRET", "SYNTHETIC-AMBIENT-CREDENTIAL");
        const auto program = root / "source-program.exe";
#ifdef _WIN32
        fs::copy_file(executable_path().parent_path() / "devbox-isolation-probe.exe", program);
        asio::io_context io;
        asio::ip::tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
#else
        fs::copy_file(executable_path(), program);
#endif
        IsolatedProgram request;
        request.private_root = root;
        request.workspace = workspace;
        request.executable = program;
        request.executable_sha256 = sha256_file(program);
#ifdef _WIN32
        request.arguments = {path_text(root / "outside-secret"),
                             std::to_string(listener.local_endpoint().port())};
        request.transition_hook = [&](std::string_view) {
            std::ofstream replaced(program, std::ios::trunc);
            require(!replaced, "executable pin blocks substitution after hash check");
            std::error_code error;
            fs::rename(workspace, root / "replaced-workspace", error);
            require(static_cast<bool>(error), "directory pins block workspace replacement before launch");
        };
        const auto output = run_isolated_program(request);
        require(output.stdout_text.find("LPAC token") != std::string::npos &&
                    read_file(workspace / "result.txt") == "isolated-workspace-output",
                "actual isolated worker ran");
        listener.non_blocking(true);
        asio::ip::tcp::socket attempted(io);
        boost::system::error_code accept_error;
        listener.accept(attempted, accept_error);
        require(accept_error == asio::error::would_block,
                "no unauthorized loopback connection reached the broker fixture");
        ensure_private_state_directory(workspace);
        for (const auto& entry : fs::directory_iterator(root))
            require(!path_text(entry.path().filename()).starts_with("worker-"),
                    "temporary broker runtime and profile retired");
        std::cout << output.stdout_text;
        {
            auto state = open_state_store(root / "state");
            GrantAuthority authority(state, root / "authority");
            GrantDefinition definition;
            definition.context = {"operator", "isolation_run", "approved_operation"};
            definition.tool = "program";
            definition.workspace = workspace;
            definition.executable = program;
            definition.executable_sha256 = request.executable_sha256;
            definition.expires_at_ms = unix_millis() + 60000;
            definition.arguments = Json{{"args", request.arguments}};
            const auto grant = authority.issue(definition);
            const auto granted =
                execute_granted_program(authority, root, grant, definition.context, definition.arguments);
            require(granted["result"]["status"] == "succeeded", "grant broker dispatches actual LPAC worker");
            write_file(workspace / "result.txt", "later edit");
            require(execute_granted_program(authority, root, grant, definition.context,
                                            definition.arguments)["replayed"] == true &&
                        read_file(workspace / "result.txt") == "later edit",
                    "durable granted effect cannot repeat after receipt replay");
        }
#else
        bool denied = false;
        try {
            run_isolated_program(request);
        } catch (const Error& error) {
            denied = std::string_view(error.what()).starts_with("ISOLATION_UNSUPPORTED");
        }
        require(denied, "unsupported platform cannot silently run an unrestricted autonomous worker");
        std::cout << "Unsupported isolation profile explicitly refused; no isolation qualification claimed\n";
#endif
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(narrow(argv[i]));
    std::vector<char*> values;
    for (auto& arg : args)
        values.push_back(arg.data());
    try {
        return run(argc, values.data());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
