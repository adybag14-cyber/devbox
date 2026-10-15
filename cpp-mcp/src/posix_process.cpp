#include "devbox/posix_process.hpp"
#ifndef _WIN32
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
#define DEVBOX_SPAWN_CLOSEFROM 1
#endif
#endif
#if defined(__ANDROID__) || defined(DEVBOX_FORCE_FORK_EXEC) ||                                               \
    (defined(__linux__) && !defined(DEVBOX_SPAWN_CLOSEFROM))
#define DEVBOX_USE_FORK_LAUNCH 1
#endif

namespace devbox {
#ifdef DEVBOX_USE_FORK_LAUNCH
namespace {
#if defined(__linux__) && defined(SYS_getdents64)
// Read the child's own descriptor table without libc directory allocation or
// locks after fork. Kernel record offsets are the linux_dirent64 ABI. A denied
// close_range must not turn a sparse table into a million close() syscalls.
bool close_listed_descriptors(unsigned first, unsigned last) noexcept {
    const int directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return false;
    std::array<char, 4096> buffer{};
    bool complete = false;
    for (;;) {
        const auto count = ::syscall(SYS_getdents64, directory, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            complete = count == 0;
            break;
        }
        const auto bytes = static_cast<std::size_t>(count);
        if (bytes > buffer.size())
            break;
        std::size_t offset = 0;
        while (offset < bytes) {
            constexpr std::size_t length_offset = 16, name_offset = 19;
            if (bytes - offset < name_offset + 1)
                goto done;
            std::uint16_t length = 0;
            std::memcpy(&length, buffer.data() + offset + length_offset, sizeof(length));
            if (length <= name_offset || length > bytes - offset)
                goto done;
            unsigned descriptor = 0;
            bool valid = true, terminated = false, digit_seen = false;
            for (std::size_t i = name_offset; i < length; ++i) {
                const auto c = buffer[offset + i];
                if (!c) {
                    terminated = true;
                    break;
                }
                if (c < '0' || c > '9' ||
                    descriptor > (static_cast<unsigned>(std::numeric_limits<int>::max()) -
                                  static_cast<unsigned>(c - '0')) /
                                     10) {
                    valid = false;
                    break;
                }
                digit_seen = true;
                descriptor = descriptor * 10 + static_cast<unsigned>(c - '0');
            }
            if (valid && terminated && digit_seen && descriptor >= first && descriptor <= last &&
                descriptor != static_cast<unsigned>(directory))
                ::close(static_cast<int>(descriptor));
            offset += length;
        }
    }
done:
    ::close(directory);
    return complete;
}
#endif
void close_child_descriptors(unsigned first, unsigned last, unsigned fallback_limit) noexcept {
    if (first > last)
        return;
#if defined(__linux__) && defined(SYS_close_range) && !defined(DEVBOX_TEST_NO_CLOSE_RANGE)
    if (::syscall(SYS_close_range, first, last, 0U) == 0)
        return;
#endif
#if defined(__linux__) && defined(SYS_getdents64)
    if (close_listed_descriptors(first, last))
        return;
#endif
    // Old Android kernels lack close_range. The bound is captured before fork;
    // this fallback performs only async-signal-safe close calls in the child.
    if (!fallback_limit)
        return;
    const auto end = std::min(last, fallback_limit - 1);
    for (auto descriptor = first; descriptor <= end; ++descriptor)
        ::close(static_cast<int>(descriptor));
}
} // namespace
#endif
pid_t spawn_posix(const fs::path& file, char* const* argv, char* const* envp, const fs::path* cwd,
                  const std::array<int, 3>& stdio, std::span<const int> close_fds, bool reset_signals) {
    const auto checked = [](int result) {
        if (result)
            throw std::system_error(result, std::generic_category());
    };
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
#ifdef DEVBOX_USE_FORK_LAUNCH
    // Android posix_spawn starts at API 28; its chdir action starts at API 34.
    // Keep the API 21 release contract without weak-linking unavailable symbols.
    int pipes[2];
#ifdef __linux__
    if (::pipe2(pipes, O_CLOEXEC) != 0)
        checked(errno);
#else
    if (::pipe(pipes) != 0)
        checked(errno);
    ::fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
#endif
    NativeHandle reader(pipes[0]), writer(pipes[1]);
    for (auto* handle : {&reader, &writer}) {
        if (handle->get() > STDERR_FILENO)
            continue;
        const auto duplicate = ::fcntl(handle->get(), F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0)
            checked(errno);
        handle->reset(duplicate);
    }
    const int error_read = reader.get(), error_write = writer.get();
    const char* executable = file.c_str();
    const char* directory = cwd ? cwd->c_str() : nullptr;
    const int* redirects = stdio.data();
    (void)close_fds;
    struct rlimit descriptor_limit{};
    if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0)
        checked(errno);
    const auto fallback_limit =
        static_cast<unsigned>(std::min<rlim_t>(descriptor_limit.rlim_cur, std::numeric_limits<int>::max()));
    struct sigaction default_signal{};
    default_signal.sa_handler = SIG_DFL;
    sigemptyset(&default_signal.sa_mask);
    const pid_t child = ::fork();
    if (child < 0)
        checked(errno);
    if (!child) {
        ::close(error_read);
        int failure = 0;
        if (::setpgid(0, 0) != 0)
            failure = errno;
        for (int fd = 0; fd < 3 && !failure; ++fd) {
            if (redirects[fd] == fd) {
                if (::fcntl(fd, F_SETFD, 0) < 0)
                    failure = errno;
            } else if (::dup2(redirects[fd], fd) < 0)
                failure = errno;
        }
        // A detached runner must never retain the parent's listening sockets,
        // locks, or another request's pipes. Preserve only stdio and the private
        // exec-error pipe, which closes atomically on successful execve.
        close_child_descriptors(3, static_cast<unsigned>(error_write) - 1, fallback_limit);
        close_child_descriptors(static_cast<unsigned>(error_write) + 1, std::numeric_limits<unsigned>::max(),
                                fallback_limit);
        if (!failure && directory && ::chdir(directory) != 0)
            failure = errno;
        if (!failure && reset_signals &&
            (::sigprocmask(SIG_SETMASK, &empty, nullptr) != 0 ||
             ::sigaction(SIGPIPE, &default_signal, nullptr) != 0))
            failure = errno;
        if (!failure) {
            ::execve(executable, argv, envp);
            failure = errno;
        }
        const auto* bytes = reinterpret_cast<const char*>(&failure);
        std::size_t written = 0;
        while (written < sizeof(failure)) {
            const auto count = ::write(error_write, bytes + written, sizeof(failure) - written);
            if (count > 0)
                written += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        ::_exit(127);
    }
    writer.reset();
    ScopeExit cleanup([&] {
        ::kill(-child, SIGKILL);
        ::kill(child, SIGKILL);
        int status;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    });
    int failure = 0;
    std::size_t received = 0;
    const auto deadline = Clock::now() + Millis(10000);
    while (received < sizeof(failure)) {
        if (Clock::now() >= deadline)
            checked(ETIMEDOUT);
        pollfd event{reader.get(), POLLIN, 0};
        const auto count = ::poll(&event, 1, 100);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            checked(errno);
        }
        if (!count)
            continue;
        const auto bytes =
            ::read(reader.get(), reinterpret_cast<char*>(&failure) + received, sizeof(failure) - received);
        if (!bytes) {
            if (received)
                checked(EIO);
            cleanup.disarm();
            return child;
        }
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            checked(errno);
        }
        received += static_cast<std::size_t>(bytes);
    }
    checked(failure ? failure : EIO);
    throw Error("Unreachable process launch state");
#else
    posix_spawn_file_actions_t actions;
    checked(posix_spawn_file_actions_init(&actions));
    ScopeExit release_actions([&] { posix_spawn_file_actions_destroy(&actions); });
    for (int fd = 0; fd < 3; ++fd)
        checked(posix_spawn_file_actions_adddup2(&actions, stdio[static_cast<std::size_t>(fd)], fd));
    for (const int fd : close_fds)
        if (fd > STDERR_FILENO)
            checked(posix_spawn_file_actions_addclose(&actions, fd));
#ifdef DEVBOX_SPAWN_CLOSEFROM
    checked(posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1));
#endif
    if (cwd)
        checked(posix_spawn_file_actions_addchdir_np(&actions, cwd->c_str()));
    posix_spawnattr_t attributes;
    checked(posix_spawnattr_init(&attributes));
    ScopeExit release_attributes([&] { posix_spawnattr_destroy(&attributes); });
    checked(posix_spawnattr_setflags(
        &attributes,
        static_cast<short>(POSIX_SPAWN_SETPGROUP |
#ifdef __APPLE__
                           POSIX_SPAWN_CLOEXEC_DEFAULT |
#endif
                           (reset_signals ? POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF : 0))));
    checked(posix_spawnattr_setpgroup(&attributes, 0));
    if (reset_signals) {
        checked(posix_spawnattr_setsigmask(&attributes, &empty));
        checked(posix_spawnattr_setsigdefault(&attributes, &defaults));
    }
    pid_t child = 0;
    checked(posix_spawn(&child, file.c_str(), &actions, &attributes, argv, envp));
    return child;
#endif
}
} // namespace devbox
#endif
