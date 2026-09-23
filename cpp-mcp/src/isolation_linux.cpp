#include "devbox/isolation.hpp"
#include "devbox/state_store.hpp"
#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace devbox {
namespace {
fs::path bubblewrap() {
    for (const auto* candidate : {"/usr/bin/bwrap", "/bin/bwrap"}) {
        std::error_code error;
        const auto path = fs::canonical(candidate, error);
        struct stat info{};
        if (!error && ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 &&
            !(info.st_mode & 0022) && ::access(path.c_str(), X_OK) == 0)
            return path;
    }
    throw Error("ISOLATION_UNAVAILABLE: install an OS-owned bubblewrap executable");
}
void private_path(const fs::path& path) {
    if (path_text(path).find('\0') != std::string::npos)
        throw Error("ISOLATION_PATH_NUL_DENIED");
    if (!path.is_absolute())
        throw Error("ISOLATION_ABSOLUTE_PATH_REQUIRED");
    auto cursor = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (part == "." || part == "..")
            throw Error("ISOLATION_PATH_ALIAS_DENIED");
        cursor /= part;
        if (fs::is_symlink(fs::symlink_status(cursor)))
            throw Error("ISOLATION_PATH_ALIAS_DENIED");
    }
}
bool below(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && relative != "." &&
           std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
}
std::vector<sock_filter> worker_filter() {
    std::vector<sock_filter> code;
    const auto statement = [&](unsigned short op, std::uint32_t value) { code.push_back({op, 0, 0, value}); };
    const auto jump = [&](unsigned short op, std::uint32_t value, unsigned char yes, unsigned char no) {
        code.push_back({op, yes, no, value});
    };
    constexpr auto load = BPF_LD | BPF_W | BPF_ABS, eq = BPF_JMP | BPF_JEQ | BPF_K;
#ifdef __x86_64__
    constexpr std::uint32_t architecture = AUDIT_ARCH_X86_64;
#else
    constexpr std::uint32_t architecture = AUDIT_ARCH_AARCH64;
#endif
    statement(load, offsetof(seccomp_data, arch));
    jump(eq, architecture, 1, 0);
    statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    statement(load, offsetof(seccomp_data, nr));
#ifdef __x86_64__
    jump(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1); // No x32 ABI bypass.
    statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif
    const auto deny = [&](std::uint32_t syscall, int error) {
        jump(eq, syscall, 0, 1);
        statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<unsigned>(error));
    };
    deny(SYS_socket, EACCES);
#ifdef SYS_socketpair
    deny(SYS_socketpair, EACCES);
#endif
#ifdef SYS_fork
    deny(SYS_fork, EPERM);
#endif
#ifdef SYS_vfork
    deny(SYS_vfork, EPERM);
#endif
#ifdef SYS_clone3
    deny(SYS_clone3, ENOSYS); // libc can fall back to the restricted thread-only clone below.
#endif
    for (const auto syscall : {SYS_unshare, SYS_setns, SYS_mount, SYS_umount2, SYS_ptrace,
                               SYS_process_vm_readv, SYS_process_vm_writev, SYS_bpf, SYS_keyctl,
                               SYS_perf_event_open, SYS_open_by_handle_at, SYS_userfaultfd})
        deny(static_cast<std::uint32_t>(syscall), EPERM);
    // Only ordinary threads may be created; process and namespace creation are refused.
    const auto branch = code.size();
    jump(eq, SYS_clone, 0, 0);
    statement(load, offsetof(seccomp_data, args[0]) + 4);
    jump(eq, 0, 1, 0);
    statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    statement(load, offsetof(seccomp_data, args[0]));
    jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0);
    statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    constexpr unsigned allowed = CLONE_THREAD | CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                                 CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
                                 CLONE_CHILD_SETTID | CLONE_SYSVSEM;
    statement(BPF_ALU | BPF_AND | BPF_K, ~allowed);
    jump(eq, 0, 1, 0);
    statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
    statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    code[branch].jf = static_cast<unsigned char>(code.size() - branch - 1);
    statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    return code;
}
void limits(const Json& spec) {
    const auto memory = json_uint(spec, "memory_bytes"), cpu_ms = json_uint(spec, "cpu_ms");
    if (memory < 64 * 1024 * 1024 || memory > 8ULL * 1024 * 1024 * 1024 || !cpu_ms || cpu_ms > 3600000)
        throw Error("ISOLATION_INVALID_BUDGET");
    const rlimit address{static_cast<rlim_t>(memory), static_cast<rlim_t>(memory)};
    const rlimit cpu{static_cast<rlim_t>((cpu_ms + 999) / 1000), static_cast<rlim_t>((cpu_ms + 999) / 1000)};
    const rlimit core{0, 0}, files{1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024}, descriptors{256, 256};
    if (::setrlimit(RLIMIT_AS, &address) || ::setrlimit(RLIMIT_CPU, &cpu) ||
        ::setrlimit(RLIMIT_CORE, &core) || ::setrlimit(RLIMIT_FSIZE, &files) ||
        ::setrlimit(RLIMIT_NOFILE, &descriptors) || ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        throw Error("ISOLATION_RESOURCE_LIMIT_DENIED");
}
} // namespace
Json linux_isolation_capabilities() {
    try {
        (void)bubblewrap();
        return Json{{"profile", "linux_bubblewrap_seccomp"},
                    {"status", "available"},
                    {"enforced_only_after_successful_launch", true},
                    {"network", "denied"},
                    {"child_processes", "denied"},
                    {"threads", "allowed"},
                    {"workspace", "private_run_workspace"},
                    {"memory_limit", "per_process_address_space"},
                    {"cpu_limit", "per_process_seconds_rounded_up"},
                    {"file_size_limit_bytes", 1073741824},
                    {"aggregate_workspace_hard_quota", false}};
    } catch (...) {
        return Json{{"profile", "linux_bubblewrap_seccomp"},
                    {"status", "unavailable"},
                    {"reason", "OS-owned bubblewrap required"}};
    }
}
int run_linux_isolation_worker(const fs::path& request_path) {
    private_path(request_path);
    ensure_private_state_directory(request_path.parent_path());
    const auto spec = read_json(request_path, 131072);
    const auto root = path_from_utf8(json_string(spec, "private_root"));
    const auto workspace = path_from_utf8(json_string(spec, "workspace"));
    const auto image = request_path.parent_path() / "program";
    private_path(root);
    private_path(workspace);
    private_path(image);
    ensure_private_state_directory(root);
    ensure_private_state_directory(workspace);
    if (!below(workspace, root) || request_path.parent_path().parent_path() != root ||
        sha256_file(image) != json_string(spec, "executable_sha256"))
        throw Error("ISOLATION_WORKER_IDENTITY_MISMATCH");
    const auto sandbox = bubblewrap();
    // Only this consumed seccomp-program FD crosses exec. No host directory or credential FD
    // reaches the untrusted program. bwrap closes its setup FDs before executing the program.
    const auto filter_path = request_path.parent_path() / "filter.bpf";
    const auto filter = worker_filter();
    write_file(filter_path, std::string_view(reinterpret_cast<const char*>(filter.data()),
                                             filter.size() * sizeof(sock_filter)));
    NativeHandle filter_fd(::open(filter_path.c_str(), O_RDONLY | O_NOFOLLOW));
    if (!filter_fd)
        throw Error("ISOLATION_FILTER_OPEN_FAILED");
    std::vector<std::string> args{
        path_text(sandbox), "--unshare-user", "--unshare-pid",     "--unshare-net", "--unshare-ipc",
        "--unshare-uts",    "--new-session",  "--die-with-parent", "--cap-drop",    "ALL",
        "--clearenv"};
    for (const auto* library : {"/lib", "/lib64", "/usr/lib", "/usr/lib64"})
        if (fs::is_directory(library))
            args.insert(args.end(), {"--ro-bind", path_text(fs::canonical(library)), library});
    if (fs::is_regular_file("/etc/ld.so.cache"))
        args.insert(args.end(), {"--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"});
    args.insert(args.end(), {"--bind",
                             path_text(workspace),
                             "/work",
                             "--ro-bind",
                             path_text(image),
                             "/app/program",
                             "--proc",
                             "/proc",
                             "--dev",
                             "/dev",
                             "--tmpfs",
                             "/tmp",
                             "--remount-ro",
                             "/",
                             "--chdir",
                             "/work",
                             "--setenv",
                             "HOME",
                             "/work",
                             "--setenv",
                             "PATH",
                             "/app",
                             "--setenv",
                             "LANG",
                             "C.UTF-8",
                             "--seccomp",
                             std::to_string(filter_fd.get()),
                             "--",
                             "/app/program"});
    const auto supplied = json_strings(spec, "args");
    if (supplied.size() > 256)
        throw Error("ISOLATION_ARGUMENT_BUDGET");
    std::size_t argument_bytes = 0;
    for (const auto& argument : supplied) {
        if (argument.find('\0') != std::string::npos)
            throw Error("ISOLATION_ARGUMENT_NUL_DENIED");
        argument_bytes += argument.size();
        if (argument_bytes > 65536)
            throw Error("ISOLATION_ARGUMENT_BUDGET");
    }
    args.insert(args.end(), supplied.begin(), supplied.end());
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    // Complete every allocation before setting the address-space limit (also works in ASan brokers).
    limits(spec);
    ::execv(sandbox.c_str(), argv.data());
    const char error[] = "ISOLATION_BUBBLEWRAP_EXEC_FAILED\n";
    const auto written = ::write(STDERR_FILENO, error, sizeof(error) - 1);
    (void)written;
    return 127;
}
ProcessOutput run_linux_isolated_program(const IsolatedProgram& request, const Cancel& cancel) {
    (void)bubblewrap();
    if (request.timeout <= Millis(0) || request.timeout > Millis(3600000) || request.output_chars > 4096 ||
        request.arguments.size() > 256 || (request.input && request.input->size() > 65536))
        throw Error("ISOLATION_INVALID_BUDGET");
    private_path(request.private_root);
    private_path(request.workspace);
    private_path(request.executable);
    ensure_private_state_directory(request.private_root);
    ensure_private_state_directory(request.workspace);
    if (!below(request.workspace, request.private_root))
        throw Error("ISOLATION_PRIVATE_WORKSPACE_REQUIRED");
    FileLock lease(request.private_root / (sha256(path_text(request.workspace)) + ".execution.lock"),
                   Millis(1000), cancel, true);
    if (!fs::is_regular_file(request.executable) || fs::hard_link_count(request.executable) != 1 ||
        fs::file_size(request.executable) > 256 * 1024 * 1024 ||
        sha256_file(request.executable) != request.executable_sha256)
        throw Error("ISOLATION_EXECUTABLE_IDENTITY_CHANGED");
    const auto runtime = request.private_root / ("worker-" + uuid());
    ensure_private_state_directory(runtime);
    ScopeExit cleanup([&] {
        // Remove only the four files created by this broker, never untrusted workspace contents.
        std::error_code error;
        for (const auto* name : {"request.json", "program", "filter.bpf"})
            fs::remove(runtime / name, error);
        fs::remove(runtime, error);
    });
    const auto image = runtime / "program", input = runtime / "request.json";
    fs::copy_file(request.executable, image, fs::copy_options::none);
    fs::permissions(image, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    if (sha256_file(image) != request.executable_sha256)
        throw Error("ISOLATION_EXECUTABLE_COPY_MISMATCH");
    write_json_atomic(input, Json{{"private_root", path_text(request.private_root)},
                                  {"workspace", path_text(request.workspace)},
                                  {"executable_sha256", request.executable_sha256},
                                  {"args", request.arguments},
                                  {"memory_bytes", request.memory_bytes},
                                  {"cpu_ms", request.cpu_ms}});
    ProcessOptions options;
    options.cwd = request.private_root;
    options.env = worker_environment();
    options.timeout = request.timeout;
    options.max_capture_chars = request.output_chars;
    options.input = request.input;
    if (request.transition_hook)
        request.transition_hook("before_launch");
    return spawn_process(path_text(executable_path()), {"--linux-isolation-worker", path_text(input)},
                         options, cancel);
}
} // namespace devbox
#else
namespace devbox {
int run_linux_isolation_worker(const fs::path&) {
    throw Error("ISOLATION_UNSUPPORTED");
}
} // namespace devbox
#endif
