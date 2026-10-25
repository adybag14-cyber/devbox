#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace devbox {
using X11Cookie = std::array<std::uint8_t, 16>;
using X11Deadline = std::chrono::steady_clock::time_point;
// Xauthority's FamilyLocal record. No wildcard or unauthenticated connection.
std::string xauthority_record(std::string_view hostname, unsigned display, const X11Cookie& cookie);
void write_xauthority(const std::filesystem::path& path, unsigned display, const X11Cookie& cookie);
// The returned descriptor is owned by the caller, nonblocking and close-on-exec.
// Only the private, same-user Unix socket is accepted; no TCP or global DISPLAY.
int connect_private_x11(unsigned display, X11Deadline deadline, const std::function<bool()>& stop,
                        std::uint32_t expected_peer_pid = 0);
bool probe_private_x11(unsigned display, const X11Cookie& cookie, X11Deadline deadline,
                       const std::function<bool()>& stop, std::uint32_t expected_peer_pid = 0);
} // namespace devbox
