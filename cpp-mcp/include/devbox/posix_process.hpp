#pragma once
#ifndef _WIN32
#include "native.hpp"
#include <array>
#include <span>

namespace devbox {
// Reserve before creating a child, so an allocation/capacity failure cannot lose ownership.
std::size_t reserve_posix_reap_slot();
void release_posix_reap_slot(std::size_t slot) noexcept;
void defer_posix_reap(std::size_t slot, pid_t child) noexcept;
// Inputs and fd tables are built before fork; the child performs only
// async-signal-safe system calls until execve or _exit.
pid_t spawn_posix(const fs::path& file, char* const* argv, char* const* envp, const fs::path* cwd,
                  const std::array<int, 3>& stdio, std::span<const int> close_fds, bool reset_signals);
} // namespace devbox
#endif
