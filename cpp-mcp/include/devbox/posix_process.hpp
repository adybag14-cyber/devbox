#pragma once
#ifndef _WIN32
#include "native.hpp"
#include <array>
#include <span>

namespace devbox {
// Inputs and fd tables are built before fork; the child performs only
// async-signal-safe system calls until execve or _exit.
pid_t spawn_posix(const fs::path& file, char* const* argv, char* const* envp, const fs::path* cwd,
                  const std::array<int, 3>& stdio, std::span<const int> close_fds, bool reset_signals);
} // namespace devbox
#endif
