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
#ifdef _WIN32
    if (argc == 2 && std::string_view(argv[1]) == "--powershell-diagnostics") {
        for (const auto* program : {"powershell.exe", "pwsh.exe"}) {
            for (const bool detached : {false, true}) {
                ProcessOptions options;
                options.env = worker_environment();
                options.timeout = Millis(8000);
                options.max_capture_chars = 1024;
                options.windows_detached_console = detached;
                if (const auto exe = find_program(program, &*options.env))
                    (*options.env)["PSModulePath"] = path_text(exe->parent_path() / "Modules");
                const auto started = Clock::now();
                Json report{{"program", program}, {"detached_console", detached}, {"first_byte_ms", nullptr}};
                NativeHandle child;
                options.on_pid = [&](std::uint32_t pid) {
                    report["pid"] = pid;
                    child.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
                };
                options.on_output = [&](OutputStream, std::string_view bytes) {
                    if (!bytes.empty() && report["first_byte_ms"].is_null())
                        report["first_byte_ms"] =
                            std::chrono::duration_cast<Millis>(Clock::now() - started).count();
                };
                try {
                    const auto output =
                        spawn_process(program,
                                      encoded_powershell_args("[Console]::Out.WriteLine('CLR-and-command-"
                                                              "ready'); Write-Output 'pipeline-ready'"),
                                      options);
                    report["status"] = output.stdout_text.find("pipeline-ready") != std::string::npos
                                           ? "completed"
                                           : "missing_expected_output";
                    report["stdout"] = output.stdout_text;
                    report["stderr"] = output.stderr_text;
                } catch (const ProcessError& error) {
                    report["status"] = error.timed_out ? "timeout" : "launch_or_exit_error";
                    report["stdout"] = error.stdout_text;
                    report["stderr"] = error.stderr_text;
                }
                report["elapsed_ms"] = std::chrono::duration_cast<Millis>(Clock::now() - started).count();
                FILETIME create{}, exit{}, kernel{}, user{};
                if (child && GetProcessTimes(child.get(), &create, &exit, &kernel, &user))
                    report["cpu_ms"] = ((std::uint64_t(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
                                        (std::uint64_t(user.dwHighDateTime) << 32) + user.dwLowDateTime) /
                                       10000;
                std::cout << report.dump() << '\n' << std::flush;
            }
        }
        return 0;
    }
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--under-parent-job") {
        ProcessOptions options;
        options.timeout = Millis(60000);
        options.max_capture_chars = 12000;
        try {
            const auto result = spawn_process(path_text(executable_path()), {"--nested-suite"}, options);
            std::cout << result.stdout_text;
            return result.exit_code;
        } catch (const ProcessError& error) {
            std::cerr << error.what() << '\n' << error.stdout_text << '\n' << error.stderr_text << '\n';
            return 1;
        }
    }
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
        // Hosted Windows images may need a cold CLR/PowerShell startup. Process deadline
        // enforcement has separate short-deadline tests; this fixture verifies successful routing.
        shell.timeout = Millis(30000);
        shell.max_capture_chars = 8192;
#ifdef _WIN32
        config->host_shell = env_or("COMSPEC", "cmd.exe");
        config->power_shell_exe = "missing-devbox-test-powershell-123.exe";
        config->power_shell_fallback_exe = "powershell.exe";
        std::cout << "[runtime] CMD override\n" << std::flush;
        shell.command = "echo runtime-cmd";
        require(trim(runtime.run_shell(shell).stdout_text) == "runtime-cmd",
                "HOST_SHELL CMD override without admin probe");
        shell.command = std::string(8100, 'x');
        rejects([&] { runtime.run_shell(shell); }, "8000 UTF-16 units");
        shell.command =
            "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); Write-Output 'powershell é😀'";
        std::cout << "[runtime] PowerShell cold fallback and Unicode\n" << std::flush;
        require(trim(runtime.run_inspection_shell(shell).stdout_text) == "powershell é😀",
                "PowerShell fallback and Unicode");
        shell.command = std::string(20000, '#') + "\nWrite-Output 'large-script-ok'";
        std::cout << "[runtime] PowerShell staged script\n" << std::flush;
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
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
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
