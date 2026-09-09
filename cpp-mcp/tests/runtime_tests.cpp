#include "devbox/native.hpp"
#include "devbox/runtime.hpp"
#include <iostream>
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class F> void rejects(F&& operation, std::string_view part) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(part) != std::string_view::npos)
            return;
        throw Error("Unexpected error: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(part));
}
int run(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child") {
        if (std::string(argv[2]) == "args")
            std::cout << Json(std::vector<std::string>(argv + 3, argv + argc)).dump();
        else if (std::string(argv[2]) == "input")
            std::cout << std::string((std::istreambuf_iterator<char>(std::cin)),
                                     std::istreambuf_iterator<char>());
        return 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-runtime-" + uuid());
    fs::create_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        auto config = std::make_shared<Config>();
        config->platform = Platform::detect();
        config->host_exec_enabled = true;
        config->runtime_mode = RuntimeMode::host;
        config->host_default_workdir = root;
        config->node_exe = path_text(executable_path());
        config->devbox_program_allowlist = {"node"};
        config->host_program_allowlist = {"node"};
        RuntimeExecutor runtime(config);
        ProgramRequest program;
        program.program = "node";
        program.args = {"--child", "input"};
        program.input = "literal stdin with quotes ' \" & $ and 😀";
        program.working_dir = root;
        program.timeout = Millis(5000);
        program.max_capture_chars = 4096;
        require(runtime.run_program(program).stdout_text == *program.input, "host program stdin");
        program.program = "forbidden";
        rejects([&] { runtime.run_program(program); }, "DEVBOX_PROGRAM_ALLOWLIST");
        rejects([&] { runtime.run_host_program_only(program); }, "HOST_PROGRAM_ALLOWLIST");
        config->host_exec_enabled = false;
        program.program = "node";
        rejects([&] { runtime.run_program(program); }, "execution is disabled");
        config->host_exec_enabled = true;
        ShellRequest shell;
        shell.working_dir = root;
        shell.timeout = Millis(10000);
        shell.max_capture_chars = 8192;
#ifdef _WIN32
        config->host_shell = env_or("COMSPEC", "cmd.exe");
        config->power_shell_exe = "missing-devbox-test-powershell-123.exe";
        config->power_shell_fallback_exe = "powershell.exe";
        shell.command = "echo runtime-cmd";
        require(trim(runtime.run_shell(shell).stdout_text) == "runtime-cmd",
                "HOST_SHELL CMD override without admin probe");
        shell.command = std::string(8100, 'x');
        rejects([&] { runtime.run_shell(shell); }, "8000 UTF-16 units");
        shell.command =
            "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); Write-Output 'powershell é😀'";
        require(trim(runtime.run_inspection_shell(shell).stdout_text) == "powershell é😀",
                "PowerShell fallback and Unicode");
        shell.command = std::string(20000, '#') + "\nWrite-Output 'large-script-ok'";
        require(trim(runtime.run_inspection_shell(shell).stdout_text) == "large-script-ok",
                "PowerShell large script staging");
        config->host_shell = config->power_shell_exe;
        shell.command = "Write-Output 'runtime-fallback'";
        // HOST_SHELL fallback applies when it equals the configured PowerShell executable.
        config->host_shell = "powershell.exe";
        require(trim(runtime.run_shell(shell).stdout_text) == "runtime-fallback", "PowerShell runtime shell");
        if (!is_administrator())
            rejects([&] { runtime.run_host_shell_only(shell); }, "already be elevated");
        const auto batch = root / "devbox-args.cmd";
        write_file(batch, "@echo off\r\n\"" + path_text(executable_path()) + "\" --child args %*\r\n");
        const auto old_path = environment("PATH");
        set_environment("PATH", path_text(root) + ";" + old_path.value_or(""));
        ScopeExit restore_path([&] { set_environment("PATH", old_path); });
        config->devbox_program_allowlist.push_back("devbox-args");
        program.program = "devbox-args";
        program.input.reset();
        program.args = {"hello world", "a&b", "%PATH%", "quoted\"value", "C:\\space path\\", ""};
        const auto batch_result = runtime.run_program(program);
        require(Json::parse(trim(batch_result.stdout_text)) == program.args,
                "batch argument quoting and environment expansion suppression");
        program.args = {"line\nbreak"};
        rejects([&] { runtime.run_program(program); }, "batch file arguments are invalid");
#else
        config->host_shell = "/bin/sh";
        shell.command = "printf 'posix-shell'";
        require(runtime.run_shell(shell).stdout_text == "posix-shell", "POSIX runtime shell");
#endif
        const auto clixml = clean_powershell_output(
            "before\n#< CLIXML\n<Objs><S "
            "S=\"Error\">bad_x000D__x000A_&lt;thing&gt;</S><Obj>progress</Obj></Objs>after");
        require(clixml == "before\nbad\r\n<thing>\nafter", "CLIXML channel and entity decoding");
        require(clean_powershell_output("#< CLIXML\n<Objs><Obj>progress</Obj></Objs>").empty(),
                "CLIXML progress suppression");
        std::cout
            << "PASS host program policy, shell overrides, Unicode, fallback, script staging and CLIXML\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i)
        arguments.push_back(narrow(wide_argv[i]));
    std::vector<char*> argv;
    for (auto& value : arguments)
        argv.push_back(value.data());
    return run(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
