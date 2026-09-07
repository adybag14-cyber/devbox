#include "devbox/native.hpp"
#include "devbox/process.hpp"
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/wait.h>
#endif
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
int child(int argc, char** argv) {
    const std::string mode = argc > 2 ? argv[2] : "";
    if (mode == "echo") {
        std::cout << Json{{"args", std::vector<std::string>(argv + 3, argv + argc)},
                          {"cwd", path_text(fs::current_path())},
                          {"environment", env_or("DEVBOX_PROCESS_TEST", "missing")}}
                         .dump();
    } else if (mode == "pipes") {
        const std::string block(8192, 'o');
        for (int i = 0; i < 128; ++i) {
            std::cout << block;
            std::cerr << std::string(8192, 'e');
        }
        std::cout.flush();
        std::cerr.flush();
        const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        std::cout << "END:" << input.size() << ':' << sha256(input);
        std::cerr << "ERR-END";
    } else if (mode == "sleep") {
        std::cout << "ready" << std::flush;
        std::this_thread::sleep_for(Millis(10000));
    } else if (mode == "tree") {
#ifdef _WIN32
        ProcessOptions options;
        options.timeout = Millis(8000);
        options.on_pid = [](std::uint32_t pid) { std::cout << pid << '\n' << std::flush; };
        try {
            spawn_process(path_text(executable_path()), {"--child", "sleep"}, options);
        } catch (...) {
            return 1;
        }
#else
        const auto executable = path_text(executable_path());
        const auto pid = ::fork();
        if (pid < 0)
            return 1;
        if (pid == 0) {
            ::execl(executable.c_str(), executable.c_str(), "--child", "sleep", nullptr);
            ::_exit(127);
        }
        std::cout << pid << '\n' << std::flush;
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
#endif
    } else if (mode == "exit") {
        std::cout << "out";
        std::cerr << "err";
        return 7;
    } else
        return 90;
    return 0;
}
int test_main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (argc > 1 && std::string(argv[1]) == "--child")
        return child(argc, argv);
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-process-" + uuid() + "-é");
    fs::create_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        CaptureAccumulator capture(6);
        capture.push("a\xf0\x9f");
        capture.push("\x98\x80"
                     "bcdef");
        capture.finish();
        auto snapshot = capture.snapshot();
        require(snapshot.original_chars == 7 && snapshot.truncated &&
                    snapshot.text == "a😀b\n... middle capture omitted 1 characters ...\ndef",
                "streaming Unicode capture");
        CaptureAccumulator partial(20);
        partial.push("\xf0\x9f");
        partial.finish();
        require(partial.snapshot().text == "�", "incomplete Unicode replacement");
        CaptureAccumulator zero(0);
        zero.push("test");
        zero.finish();
        require(zero.snapshot().text.empty() && zero.snapshot().original_chars == 4 &&
                    zero.snapshot().truncated,
                "zero capture bound");
        const auto self = path_text(executable_path());
        require(!process_alive(0) && !process_instance(0), "PID zero exclusion");
        const auto identity = process_instance(process_id());
        require(identity && process_matches_instance(process_id(), identity) &&
                    !process_matches_instance(process_id(), *identity + 1),
                "stable identity and mismatch");
        require(!terminate_process_tree(process_id(), identity), "self termination refusal");
        ProcessOptions options;
        options.cwd = root;
        options.timeout = Millis(5000);
        options.max_capture_chars = 8192;
        options.env = current_environment();
        (*options.env)["DEVBOX_PROCESS_TEST"] = "isolated=child é";
        const std::vector<std::string> values{"",    "hello world", "x\"y",       "C:\\space path\\",
                                              "é😀", "a&b",         "line\nbreak"};
        auto arguments = std::vector<std::string>{"--child", "echo"};
        arguments.insert(arguments.end(), values.begin(), values.end());
        auto output = spawn_process(self, arguments, options);
        const auto echoed = Json::parse(output.stdout_text);
        require(echoed["args"] == values, "argument preservation");
        require(echoed["environment"] == "isolated=child é", "isolated child environment");
        require(fs::equivalent(path_from_utf8(echoed["cwd"].get<std::string>()), root),
                "child working directory");
        require(environment("DEVBOX_PROCESS_TEST") != "isolated=child é", "parent environment unchanged");
        std::cout << "PASS identity, arguments, cwd, environment\n" << std::flush;
        options.input = std::string(1024 * 1024, 'i');
        options.max_capture_chars = 256;
        std::size_t stdout_bytes = 0, stderr_bytes = 0;
        options.on_output = [&](OutputStream stream, std::string_view chunk) {
            (stream == OutputStream::stdout_stream ? stdout_bytes : stderr_bytes) += chunk.size();
        };
        output = spawn_process(self, {"--child", "pipes"}, options);
        require(stdout_bytes > 1024 * 1024 && stderr_bytes > 1024 * 1024, "concurrent stream callbacks");
        require(output.stdout_capture_truncated && output.stderr_capture_truncated,
                "bounded process capture");
        require(output.stdout_text.ends_with("END:1048576:" + sha256(*options.input)) &&
                    output.stderr_text.ends_with("ERR-END"),
                "large stdin and output tails");
        require(output.stdout_text.size() < 400 && output.stderr_text.size() < 400, "retained capture bound");
        std::cout << "PASS simultaneous 1 MiB stdin/stdout/stderr and capture bounds\n" << std::flush;
        options.input.reset();
        options.on_output = {};
        options.max_capture_chars = 1024;
        bool failed = false;
        try {
            spawn_process(self, {"--child", "exit"}, options);
        } catch (const ProcessError& error) {
            failed = error.exit_code == 7 && error.stdout_text == "out" && error.stderr_text == "err" &&
                     std::string(error.what()) == "err";
        }
        require(failed, "nonzero process error envelope");
        failed = false;
        try {
            spawn_process(path_text(root / "missing-program-123.exe"), {}, options);
        } catch (const ProcessError& error) {
            failed = !error.exit_code && !error.aborted && !error.timed_out;
        }
        require(failed, "launch failure classification");
        std::uint32_t child_pid = 0;
        options.on_pid = [&](std::uint32_t pid) { child_pid = pid; };
        options.timeout = Millis(250);
        failed = false;
        const auto before = Clock::now();
        try {
            spawn_process(self, {"--child", "sleep"}, options);
        } catch (const ProcessError& error) {
            failed = error.timed_out && !error.aborted;
        }
        require(failed && child_pid && !process_alive(child_pid) && Clock::now() - before < Millis(4000),
                "timeout terminates owned child");
        options.timeout = Millis(5000);
        auto cancel = std::make_shared<Cancellation>();
        std::jthread canceller([&] {
            cancel->wait_for(Millis(200));
            cancel->cancel();
        });
        failed = false;
        try {
            spawn_process(self, {"--child", "sleep"}, options, cancel);
        } catch (const ProcessError& error) {
            failed = error.aborted && !error.timed_out;
        }
        require(failed && !process_alive(child_pid), "cancellation terminates owned child");
        std::uint32_t grandchild = 0;
        auto tree_cancel = std::make_shared<Cancellation>();
        options.on_output = [&](OutputStream stream, std::string_view chunk) {
            if (stream == OutputStream::stdout_stream && !grandchild) {
                const auto value = trim(chunk);
                if (!value.empty()) {
                    grandchild = static_cast<std::uint32_t>(std::stoul(value));
                    tree_cancel->cancel();
                }
            }
        };
        failed = false;
        try {
            spawn_process(self, {"--child", "tree"}, options, tree_cancel);
        } catch (const ProcessError& error) {
            failed = error.aborted;
        }
        require(failed && grandchild && !process_alive(child_pid), "process tree cancellation");
        const auto deadline = Clock::now() + Millis(2000);
        while (process_alive(grandchild) && Clock::now() < deadline)
            std::this_thread::sleep_for(Millis(10));
        require(!process_alive(grandchild), "no live grandchild after cancellation");
        std::cout << "PASS launch errors, exit codes, deadlines, cancellation and descendants\n";
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
    return test_main(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    return test_main(argc, argv);
}
#endif
