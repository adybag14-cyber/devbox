#include "devbox/native.hpp"
#include "devbox/posix_process.hpp"
#include "devbox/process.hpp"
#include "devbox/scoped_thread.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
int child(int argc, char** argv) {
    const std::string mode = argc > 2 ? argv[2] : "";
#ifdef _WIN32
    if (mode == "abrupt-owner" && argc == 4) {
        Json owned;
        ProcessOptions options;
        options.timeout = Millis(5000);
        options.on_pid = [&](std::uint32_t pid) {
            owned = Json{{"pid", pid}, {"instance", *process_instance(pid)}};
        };
        options.on_output = [&](OutputStream, std::string_view text) {
            if (text.find("ready") != text.npos) {
                write_json_atomic(path_from_utf8(argv[3]), owned);
                std::_Exit(0);
            }
        };
        spawn_process(path_text(executable_path()), {"--child", "sleep"}, options);
        return 1;
    }
#endif
    if (mode == "echo") {
        std::cout << Json{{"args", std::vector<std::string>(argv + 3, argv + argc)},
                          {"cwd", path_text(fs::current_path())},
                          {"environment", env_or("DEVBOX_PROCESS_TEST", "missing")}}
                         .dump();
    } else if (mode == "environment") {
        std::cout << Json(current_environment()).dump();
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
    } else if (mode == "stdin") {
        const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        std::cout << input.size();
    } else if (mode == "exit") {
        std::cout << "out";
        std::cerr << "err";
        return 7;
#ifndef _WIN32
    } else if (mode == "fd-identity" && argc >= 6) {
        struct stat descriptor{};
        const bool inherited = ::fstat(std::stoi(argv[3]), &descriptor) == 0 &&
                               static_cast<std::uint64_t>(descriptor.st_dev) == std::stoull(argv[4]) &&
                               static_cast<std::uint64_t>(descriptor.st_ino) == std::stoull(argv[5]);
        const auto report = Json{{"inherited", inherited}}.dump();
        if (argc == 7)
            write_file(path_from_utf8(argv[6]), report);
        else
            std::cout << report;
#endif
    } else
        return 90;
    return 0;
}
int test_main(int argc, char** argv) {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1) {
        std::cerr << "Cannot configure binary fixture standard streams\n";
        return 2;
    }
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
#ifndef _WIN32
        {
            std::vector<std::size_t> slots;
            ScopeExit release([&] {
                for (const auto slot : slots)
                    release_posix_reap_slot(slot);
            });
            for (int i = 0; i < 4096; ++i)
                slots.push_back(reserve_posix_reap_slot());
            bool full = false;
            try {
                (void)reserve_posix_reap_slot();
            } catch (const Error&) {
                full = true;
            }
            require(full, "bounded reaper admission refuses before an untrackable child is launched");
        }
        {
            const auto slot = reserve_posix_reap_slot();
            ScopeExit release([&] { release_posix_reap_slot(slot); });
            const auto pid = ::fork();
            require(pid >= 0, "reaper fixture fork");
            if (pid == 0) {
                const timespec delay{0, 60000000};
                ::nanosleep(&delay, nullptr);
                ::_exit(0);
            }
            defer_posix_reap(slot, pid);
            release.disarm();
            const auto deadline = Clock::now() + Millis(2000);
            bool reaped = false;
            while (Clock::now() < deadline) {
                siginfo_t observed{};
                if (::waitid(P_PID, static_cast<id_t>(pid), &observed, WEXITED | WNOHANG | WNOWAIT) < 0 &&
                    errno == ECHILD) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(Millis(5));
            }
            require(reaped, "deferred child collected without a blocking shutdown waitpid");
        }
#endif
        for (const auto input : {std::optional<std::string>(), std::optional<std::string>("")}) {
            ProcessOptions options;
            options.input = input;
            options.timeout = Millis(2000);
            require(spawn_process(path_text(executable_path()), {"--child", "stdin"}, options).stdout_text ==
                        "0",
                    "absent and empty stdin reach EOF without hanging the child");
        }
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
#ifdef _WIN32
        {
            const auto receipt = root / "orphan-fixture.json";
            const auto owner =
                spawn_detached(executable_path(), {"--child", "abrupt-owner", path_text(receipt)}, root);
            const auto owner_instance = process_instance(owner);
            std::optional<Json> orphan;
            ScopeExit cleanup_owner([&] {
                if (owner_instance && process_matches_instance(owner, owner_instance))
                    terminate_process_tree(owner, owner_instance);
                if (orphan)
                    terminate_process_tree(static_cast<std::uint32_t>(json_uint(*orphan, "pid")),
                                           json_uint(*orphan, "instance"));
            });
            const auto deadline = Clock::now() + Millis(5000);
            while (!fs::exists(receipt) && Clock::now() < deadline)
                std::this_thread::sleep_for(Millis(5));
            require(fs::exists(receipt), "abrupt owner fixture published the exact child identity");
            orphan = read_json(receipt);
            const auto pid = static_cast<std::uint32_t>(json_uint(*orphan, "pid"));
            const auto instance = json_uint(*orphan, "instance");
            while (process_matches_instance(pid, instance) && Clock::now() < deadline)
                std::this_thread::sleep_for(Millis(5));
            require(!process_matches_instance(pid, instance),
                    "foreground child is terminated when its owner exits abruptly");
        }
#endif
        {
            const auto prior = environment("DEVBOX_AUDIT_CANARY");
            const auto prior_token = environment("OPENAI_API_KEY");
            const auto prior_identity = environment("USERNAME");
            ScopeExit restore([&] {
                set_environment("DEVBOX_AUDIT_CANARY", prior);
                set_environment("OPENAI_API_KEY", prior_token);
                set_environment("USERNAME", prior_identity);
            });
            set_environment("DEVBOX_AUDIT_CANARY", "SYNTHETIC-CREDENTIAL-NEVER-INHERIT");
            set_environment("OPENAI_API_KEY", "SYNTHETIC-PROVIDER-KEY-NEVER-INHERIT");
            set_environment("USERNAME", "devbox-public-runtime-identity");
            const auto child_env = spawn_process(self, {"--child", "environment"}).stdout_text;
            require(child_env.find("SYNTHETIC-") == std::string::npos &&
                        child_env.find("DEVBOX_AUDIT_CANARY") == std::string::npos,
                    "generic child environments exclude arbitrary and known credential variables");
            require(child_env.find("devbox-public-runtime-identity") != std::string::npos,
                    "standard nonsecret runtime identity remains available without inheriting credentials");
            ProcessOptions granted;
            granted.env = worker_environment();
            (*granted.env)["DEVBOX_EXPLICIT_GRANT_FIXTURE"] = "explicitly-authorized-fixture";
            require(spawn_process(self, {"--child", "environment"}, granted)
                            .stdout_text.find("explicitly-authorized-fixture") != std::string::npos,
                    "dedicated callers can explicitly supply a scoped environment");
        }
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
#ifndef _WIN32
        const auto private_path = root / "parent-private-descriptor";
        write_file(private_path, "parent-only");
        NativeHandle private_fd(::open(private_path.c_str(), O_RDONLY));
        struct stat private_info{};
        require(private_fd && ::fstat(private_fd.get(), &private_info) == 0, "private descriptor fixture");
        const std::vector<std::string> descriptor_args{
            "--child", "fd-identity", std::to_string(private_fd.get()), std::to_string(private_info.st_dev),
            std::to_string(private_info.st_ino)};
        const auto descriptor = spawn_process(self, descriptor_args, options);
        require(!json_bool(Json::parse(descriptor.stdout_text), "inherited"),
                "foreground child inherited an unrelated parent descriptor");
        auto detached_args = descriptor_args;
        const auto report = root / "detached-fd-report.json";
        detached_args.push_back(path_text(report));
        const auto detached = spawn_detached(executable_path(), detached_args, root, options.env);
        const auto descriptor_deadline = Clock::now() + Millis(3000);
        while ((!fs::exists(report) || process_alive(detached)) && Clock::now() < descriptor_deadline)
            std::this_thread::sleep_for(Millis(10));
        require(!process_alive(detached), "descriptor fixture child did not finish");
        require(!json_bool(read_json(report), "inherited"),
                "detached child inherited an unrelated parent descriptor");
        std::cout << "PASS child descriptor isolation\n";
#endif
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
        auto never_started = std::make_shared<Cancellation>();
        never_started->cancel();
        failed = false;
        try {
            spawn_process(self, {"--child", "sleep"}, options, never_started);
        } catch (const ProcessError& error) {
            failed = error.aborted && error.process_started == false && !error.exit_code && !error.signal;
        }
        require(failed && !child_pid, "pre-launch cancellation has explicit proof that no child started");
        options.timeout = Millis(250);
        failed = false;
        const auto before = Clock::now();
        try {
            spawn_process(self, {"--child", "sleep"}, options);
        } catch (const ProcessError& error) {
            failed = error.timed_out && !error.aborted && error.process_started == true;
        }
        require(failed && child_pid && !process_alive(child_pid) && Clock::now() - before < Millis(4000),
                "timeout terminates owned child");
        options.timeout = Millis(5000);
        auto cancel = std::make_shared<Cancellation>();
        ScopedThread canceller([&] {
            cancel->wait_for(Millis(200));
            cancel->cancel();
        });
        failed = false;
        try {
            spawn_process(self, {"--child", "sleep"}, options, cancel);
        } catch (const ProcessError& error) {
            failed = error.aborted && !error.timed_out && error.process_started == true;
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
        {
            ProcessOptions observer;
            observer.timeout = Millis(3000);
            std::uint32_t owned_pid = 0;
            std::optional<std::uint64_t> owned_instance;
            observer.on_pid = [&](std::uint32_t pid) {
                owned_pid = pid;
                owned_instance = process_instance(pid);
            };
            observer.on_output = [](OutputStream, std::string_view) {
                throw Error("controlled observer failure");
            };
            bool unknown = false;
            try {
                spawn_process(self, {"--child", "sleep"}, observer);
            } catch (const ProcessError& error) {
                unknown = error.process_started == true && !error.exit_code && !error.signal;
            }
            require(unknown && owned_pid && owned_instance,
                    "post-launch observer failure retains launch evidence without inventing exit status");
            const auto until = Clock::now() + Millis(2000);
            while (process_matches_instance(owned_pid, owned_instance) && Clock::now() < until)
                std::this_thread::sleep_for(Millis(5));
            require(!process_matches_instance(owned_pid, owned_instance),
                    "owned observer-failure child is eventually reaped");
        }
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
