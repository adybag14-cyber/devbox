#include "devbox/native.hpp"
#include "devbox/process.hpp"
#include <algorithm>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif
namespace devbox {
#ifndef _WIN32
namespace {
class ChildReaper {
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<pid_t> children_;
    std::jthread thread_;

  public:
    ChildReaper()
        : thread_([this](std::stop_token stop) {
              std::unique_lock lock(mutex_);
              while (!stop.stop_requested()) {
                  std::erase_if(children_, [](pid_t pid) {
                      int status = 0;
                      const auto result = ::waitpid(pid, &status, WNOHANG);
                      return result == pid || (result < 0 && errno == ECHILD);
                  });
                  wake_.wait_for(lock, Millis(100));
              }
          }) {}
    ~ChildReaper() {
        thread_.request_stop();
        wake_.notify_all();
        thread_.join();
    }
    void add(pid_t pid) {
        std::lock_guard lock(mutex_);
        children_.push_back(pid);
        wake_.notify_one();
    }
};
} // namespace
#endif
std::uint32_t spawn_detached(const fs::path& file, const std::vector<std::string>& args, const fs::path& cwd,
                             const std::optional<Environment>& env) {
    for (const auto& argument : args)
        if (argument.find('\0') != std::string::npos)
            throw Error("Detached arguments cannot contain NUL bytes");
#ifdef _WIN32
    const auto program = file.wstring();
    auto command_text = quote_windows_argument(path_text(file));
    for (const auto& argument : args)
        command_text += " " + quote_windows_argument(argument);
    auto command = wide(command_text);
    if (command.size() >= 32767)
        throw Error("Detached command line exceeds the Windows limit");
    std::vector<wchar_t> environment_block;
    if (env) {
        std::vector<std::wstring> entries;
        for (const auto& [key, value] : *env) {
            if (key.empty() || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos ||
                key.find('=', key.starts_with('=') ? 1 : 0) != std::string::npos)
                throw Error("Invalid detached environment");
            entries.push_back(wide(key + "=" + value));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const auto& entry : entries) {
            environment_block.insert(environment_block.end(), entry.begin(), entry.end());
            environment_block.push_back(0);
        }
        environment_block.push_back(0);
        if (entries.empty())
            environment_block.push_back(0);
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    NativeHandle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null)
        throw Error(windows_error());
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        throw Error(windows_error());
    ScopeExit cleanup([&] { DeleteProcThreadAttributeList(attributes); });
    auto handle = null.get();
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &handle, sizeof(handle),
                                   nullptr, nullptr))
        throw Error(windows_error());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handle;
    startup.StartupInfo.hStdOutput = handle;
    startup.StartupInfo.hStdError = handle;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        env ? environment_block.data() : nullptr, cwd.c_str(), &startup.StartupInfo,
                        &information))
        throw Error(windows_error());
    NativeHandle process(information.hProcess), thread(information.hThread);
    return information.dwProcessId;
#else
    const auto checked = [](int result) {
        if (result)
            throw std::system_error(result, std::generic_category());
    };
    NativeHandle null(::open("/dev/null", O_RDWR | O_CLOEXEC));
    if (!null)
        throw std::system_error(errno, std::generic_category());
    posix_spawn_file_actions_t actions;
    checked(posix_spawn_file_actions_init(&actions));
    ScopeExit release_actions([&] { posix_spawn_file_actions_destroy(&actions); });
    for (int fd = 0; fd < 3; ++fd)
        checked(posix_spawn_file_actions_adddup2(&actions, null.get(), fd));
    if (null.get() > 2)
        checked(posix_spawn_file_actions_addclose(&actions, null.get()));
    checked(posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str()));
    posix_spawnattr_t attributes;
    checked(posix_spawnattr_init(&attributes));
    ScopeExit release_attributes([&] { posix_spawnattr_destroy(&attributes); });
    checked(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    checked(posix_spawnattr_setpgroup(&attributes, 0));
    std::vector<std::string> arguments{path_text(file)};
    arguments.insert(arguments.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& value : arguments)
        argv.push_back(value.data());
    argv.push_back(nullptr);
    std::vector<std::string> entries;
    std::vector<char*> envp;
    if (env) {
        for (const auto& [key, value] : *env) {
            if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
                value.find('\0') != std::string::npos)
                throw Error("Invalid detached environment");
            entries.push_back(key + "=" + value);
        }
        for (auto& entry : entries)
            envp.push_back(entry.data());
        envp.push_back(nullptr);
    }
    // Initialize the reaper before spawning so an allocation failure cannot lose a child.
    static ChildReaper reaper;
    pid_t pid = 0;
    checked(posix_spawn(&pid, file.c_str(), &actions, &attributes, argv.data(), env ? envp.data() : environ));
    reaper.add(pid);
    return static_cast<std::uint32_t>(pid);
#endif
}
} // namespace devbox
