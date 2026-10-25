#include "devbox/x11_session.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace devbox {
namespace {
void number(std::string& out, unsigned value) {
    out.push_back(static_cast<char>((value >> 8) & 255));
    out.push_back(static_cast<char>(value & 255));
}
void field(std::string& out, std::string_view value) {
    number(out, static_cast<unsigned>(value.size()));
    out.append(value);
}
void validate_display(unsigned display) {
    if (display > 65535)
        throw std::invalid_argument("DESKTOP_DISPLAY_RANGE");
}
#if defined(__linux__) && !defined(__ANDROID__)
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
void check(X11Deadline deadline, const std::function<bool()>& stop) {
    if ((stop && stop()) || std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("DESKTOP_X11_DEADLINE_OR_CANCELLED");
}
void wait(int fd, short events, X11Deadline deadline, const std::function<bool()>& stop) {
    for (;;) {
        check(deadline, stop);
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        pollfd poller{fd, events, 0};
        const auto result = ::poll(&poller, 1, static_cast<int>(std::clamp<std::int64_t>(left, 1, 50)));
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("DESKTOP_X11_POLL_FAILED");
        if (result > 0) {
            if (poller.revents & events)
                return;
            if (poller.revents & (POLLERR | POLLHUP | POLLNVAL))
                throw std::runtime_error("DESKTOP_X11_DISCONNECTED");
        }
    }
}
void transfer(int fd, char* data, std::size_t bytes, bool sending, X11Deadline deadline,
              const std::function<bool()>& stop) {
    while (bytes) {
        check(deadline, stop);
        const auto count = sending ? ::send(fd, data, bytes, MSG_NOSIGNAL) : ::recv(fd, data, bytes, 0);
        if (count > 0) {
            data += count;
            bytes -= static_cast<std::size_t>(count);
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait(fd, sending ? POLLOUT : POLLIN, deadline, stop);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("DESKTOP_X11_DISCONNECTED");
        }
    }
}
void validate_file(int directory, const char* name) {
    struct stat state{};
    if (::fstatat(directory, name, &state, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
            return;
        throw std::runtime_error("DESKTOP_AUTHORITY_INSPECT_FAILED");
    }
    if (!S_ISREG(state.st_mode) || state.st_uid != ::geteuid() || state.st_nlink != 1 ||
        (state.st_mode & 077) != 0)
        throw std::runtime_error("DESKTOP_AUTHORITY_UNSAFE_FILE");
}
#endif
} // namespace
std::string xauthority_record(std::string_view hostname, unsigned display, const X11Cookie& cookie) {
    validate_display(display);
    if (hostname.empty() || hostname.size() > 255 || hostname.find('\0') != hostname.npos)
        throw std::invalid_argument("DESKTOP_HOSTNAME_INVALID");
    std::string output;
    output.reserve(hostname.size() + 64);
    number(output, 256); // FamilyLocal, X11/Xauth.h.
    field(output, hostname);
    field(output, std::to_string(display));
    field(output, "MIT-MAGIC-COOKIE-1");
    field(output, {reinterpret_cast<const char*>(cookie.data()), cookie.size()});
    return output;
}
void write_xauthority(const std::filesystem::path& path, unsigned display, const X11Cookie& cookie) {
#if defined(__linux__) && !defined(__ANDROID__)
    std::array<char, 256> hostname{};
    if (::gethostname(hostname.data(), hostname.size()) != 0 || hostname.back() != '\0')
        throw std::runtime_error("DESKTOP_HOSTNAME_UNAVAILABLE");
    const auto data = xauthority_record(hostname.data(), display, cookie);
    const auto name = path.filename().string();
    if (name.empty() || name == "." || name == "..")
        throw std::invalid_argument("DESKTOP_AUTHORITY_FILENAME");
    Fd directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat state{};
    if (directory.value < 0 || ::fstat(directory.value, &state) != 0 || !S_ISDIR(state.st_mode) ||
        state.st_uid != ::geteuid() || (state.st_mode & 077) != 0)
        throw std::runtime_error("DESKTOP_AUTHORITY_PRIVATE_PARENT_REQUIRED");
    validate_file(directory.value, name.c_str());
    static std::atomic<unsigned long long> sequence{0};
    const auto staged = ".authority-" + std::to_string(::getpid()) + "-" + std::to_string(++sequence);
    Fd output(::openat(directory.value, staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                       0600));
    if (output.value < 0)
        throw std::runtime_error("DESKTOP_AUTHORITY_CREATE_FAILED");
    struct Cleanup {
        int directory;
        const std::string& name;
        ~Cleanup() {
            ::unlinkat(directory, name.c_str(), 0);
        }
    } cleanup{directory.value, staged};
    if (::fchmod(output.value, 0600) != 0)
        throw std::runtime_error("DESKTOP_AUTHORITY_PERMISSIONS_FAILED");
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(output.value, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("DESKTOP_AUTHORITY_WRITE_FAILED");
        offset += static_cast<std::size_t>(count);
    }
    if (::fstat(output.value, &state) != 0 || !S_ISREG(state.st_mode) || state.st_uid != ::geteuid() ||
        state.st_nlink != 1 || (state.st_mode & 077) != 0 || ::fsync(output.value) != 0)
        throw std::runtime_error("DESKTOP_AUTHORITY_SYNC_FAILED");
    validate_file(directory.value, name.c_str());
    if (::renameat(directory.value, staged.c_str(), directory.value, name.c_str()) != 0 ||
        ::fsync(directory.value) != 0)
        throw std::runtime_error("DESKTOP_AUTHORITY_REPLACE_FAILED");
#else
    (void)path;
    (void)display;
    (void)cookie;
    throw std::runtime_error("Native Xauthority requires Linux");
#endif
}
int connect_private_x11(unsigned display, X11Deadline deadline, const std::function<bool()>& stop) {
#if defined(__linux__) && !defined(__ANDROID__)
    validate_display(display);
    check(deadline, stop);
    const auto path = "/tmp/.X11-unix/X" + std::to_string(display);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(path.begin(), path.end(), address.sun_path);
    Fd socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket.value < 0)
        throw std::runtime_error("DESKTOP_X11_SOCKET_FAILED");
    auto connected = ::connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    // WSLg can expose a read-only /tmp/.X11-unix directory. Xvfb still owns
    // its Linux abstract socket; authenticate that same-user endpoint too.
    if (connected != 0 && (errno == ENOENT || errno == ECONNREFUSED || errno == EACCES)) {
        std::fill(std::begin(address.sun_path), std::end(address.sun_path), '\0');
        std::copy(path.begin(), path.end(), address.sun_path + 1);
        connected = ::connect(socket.value, reinterpret_cast<sockaddr*>(&address),
                              static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1));
    }
    if (connected != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN)
            throw std::runtime_error("DESKTOP_X11_NOT_READY");
        wait(socket.value, POLLOUT, deadline, stop);
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
            throw std::runtime_error("DESKTOP_X11_NOT_READY");
    }
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(socket.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid())
        throw std::runtime_error("DESKTOP_X11_PRIVATE_PEER_REQUIRED");
    const auto result = socket.value;
    socket.value = -1;
    return result;
#else
    (void)display;
    (void)deadline;
    (void)stop;
    throw std::runtime_error("Native X11 requires Linux");
#endif
}
bool probe_private_x11(unsigned display, const X11Cookie& cookie, X11Deadline deadline,
                       const std::function<bool()>& stop) {
#if defined(__linux__) && !defined(__ANDROID__)
    Fd socket(connect_private_x11(display, deadline, stop));
    // X11 setup uses little-endian lengths; the authority file uses big-endian.
    std::array<char, 48> setup{};
    setup[0] = 'l';
    setup[2] = 11;
    setup[6] = 18;
    setup[8] = 16;
    constexpr std::string_view name = "MIT-MAGIC-COOKIE-1";
    std::copy(name.begin(), name.end(), setup.begin() + 12);
    std::copy(cookie.begin(), cookie.end(), setup.begin() + 32);
    transfer(socket.value, setup.data(), setup.size(), true, deadline, stop);
    std::array<char, 8> reply{};
    transfer(socket.value, reply.data(), reply.size(), false, deadline, stop);
    const auto length = static_cast<unsigned char>(reply[6]) | (static_cast<unsigned char>(reply[7]) << 8);
    // A private single-screen Xvfb setup is tiny. Refuse a malicious peer's
    // oversized greeting rather than allocating an unbounded protocol body.
    if (length > 16384)
        throw std::runtime_error("DESKTOP_X11_SETUP_LIMIT");
    std::vector<char> body(static_cast<std::size_t>(length) * 4);
    transfer(socket.value, body.data(), body.size(), false, deadline, stop);
    return reply[0] == 1 && reply[2] == 11 && reply[3] == 0 && body.size() >= 32;
#else
    (void)display;
    (void)cookie;
    (void)deadline;
    (void)stop;
    throw std::runtime_error("Native X11 requires Linux");
#endif
}
} // namespace devbox
