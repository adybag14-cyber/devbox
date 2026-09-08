#include "devbox/posix_process.hpp"
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

namespace devbox {
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
#if (defined(__ANDROID__) && __ANDROID_API__ < 34) || defined(DEVBOX_FORCE_FORK_EXEC)
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
    const int* closes = close_fds.data();
    const auto close_count = close_fds.size();
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
        for (std::size_t i = 0; i < close_count; ++i)
            if (closes[i] > STDERR_FILENO && closes[i] != error_write)
                ::close(closes[i]);
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
    if (cwd)
        checked(posix_spawn_file_actions_addchdir_np(&actions, cwd->c_str()));
    posix_spawnattr_t attributes;
    checked(posix_spawnattr_init(&attributes));
    ScopeExit release_attributes([&] { posix_spawnattr_destroy(&attributes); });
    checked(posix_spawnattr_setflags(
        &attributes,
        static_cast<short>(POSIX_SPAWN_SETPGROUP |
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
