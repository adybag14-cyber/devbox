#include "devbox/x11_session.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
using namespace devbox;
#ifdef DEVBOX_TEST_NATIVE_WM
void check_window_manager(unsigned display, const X11Cookie& cookie, int server_pid);
#endif
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename F> void rejects(F&& fn, const char* message) {
    bool caught = false;
    try {
        fn();
    } catch (const std::exception&) {
        caught = true;
    }
    require(caught, message);
}
#if defined(__linux__) && !defined(__ANDROID__)
struct Child {
    pid_t pid = -1;
    ~Child() {
        if (pid <= 0)
            return;
        // This unreaped direct child cannot have its PID reused. No shared
        // display or process-name matching is used for fixture cleanup.
        int result = 0;
        if (::waitpid(pid, &result, WNOHANG) == pid)
            return;
        ::kill(pid, SIGTERM);
        for (unsigned i = 0; i < 100; ++i) {
            const auto waited = ::waitpid(pid, &result, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &result, 0) < 0 && errno == EINTR) {
        }
    }
};
void live(const char* xvfb, const X11Cookie& cookie) {
    namespace fs = std::filesystem;
    auto pattern = (fs::temp_directory_path() / "devbox-x11-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "Private fixture directory");
    const fs::path root(pattern), authority = root / "authority";
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    } cleanup{root};
    // Never remove a preexisting server socket or lock. An occupied display
    // is skipped, and Xvfb also arbitrates races through its own lock.
    unsigned display = 20000u + static_cast<unsigned>(::getpid() % 40000);
    for (unsigned count = 0; count < 100; ++count, ++display)
        if (!fs::exists("/tmp/.X11-unix/X" + std::to_string(display)) &&
            !fs::exists("/tmp/.X" + std::to_string(display) + "-lock"))
            break;
    {
        const auto prior_mask = ::umask(0777);
        struct RestoreMask {
            mode_t value;
            ~RestoreMask() {
                ::umask(value);
            }
        } restore_mask{prior_mask};
        write_xauthority(authority, display, cookie);
    }
    struct stat state{};
    require(::stat(authority.c_str(), &state) == 0 && (state.st_mode & 0777) == 0600 && state.st_nlink == 1,
            "Authority permissions");
    const auto original = fs::file_size(authority);
    fs::create_hard_link(authority, root / "hard-link");
    rejects([&] { write_xauthority(authority, display, cookie); }, "Hard-linked authority accepted");
    fs::remove(root / "hard-link");
    fs::rename(authority, root / "original");
    fs::create_symlink(root / "original", authority);
    rejects([&] { write_xauthority(authority, display, cookie); }, "Symlink authority accepted");
    require(fs::file_size(root / "original") == original, "Unsafe target modified");
    fs::remove(authority);
    write_xauthority(authority, display, cookie);
    std::string name = ":" + std::to_string(display), file = authority.string();
    std::array<char*, 11> args{const_cast<char*>(xvfb),
                               name.data(),
                               const_cast<char*>("-screen"),
                               const_cast<char*>("0"),
                               const_cast<char*>("640x480x24"),
                               const_cast<char*>("-nolisten"),
                               const_cast<char*>("tcp"),
                               const_cast<char*>("-auth"),
                               file.data(),
                               const_cast<char*>("-noreset"),
                               nullptr};
    Child server;
    require(::posix_spawn(&server.pid, xvfb, nullptr, nullptr, args.data(), environ) == 0,
            "Start owned Xvfb");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool ready = false;
    while (std::chrono::steady_clock::now() < until && !ready) {
        try {
            ready = probe_private_x11(display, cookie,
                                      std::chrono::steady_clock::now() + std::chrono::milliseconds(100), {});
        } catch (const std::exception&) {
        }
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(ready, "Xvfb did not accept native authority and handshake");
    auto wrong = cookie;
    wrong[0] ^= 255;
    require(
        !probe_private_x11(display, wrong, std::chrono::steady_clock::now() + std::chrono::seconds(1), {}),
        "Xvfb accepted incorrect cookie");
    rejects([&] { (void)probe_private_x11(display, cookie, until, [] { return true; }); },
            "X11 cancellation ignored");
    // Helpers cannot be found through PATH during the protocol operation.
    ::setenv("PATH", "", 1);
    require(
        probe_private_x11(display, cookie, std::chrono::steady_clock::now() + std::chrono::seconds(1), {}),
        "Native handshake depended on host helper PATH");
#ifdef DEVBOX_TEST_NATIVE_WM
    check_window_manager(display, cookie, server.pid);
#endif
}
#endif
} // namespace
int main(int argc, char** argv) {
    try {
        X11Cookie cookie{};
        for (unsigned i = 0; i < cookie.size(); ++i)
            cookie[i] = static_cast<std::uint8_t>(i);
        const auto record = xauthority_record("test", 99, cookie);
        const std::array<unsigned char, 50> expected{
            1,   0,   0,   4,   't', 'e', 's', 't', 0,   2,   '9', '9', 0,   18,  'M', 'I', 'T',
            '-', 'M', 'A', 'G', 'I', 'C', '-', 'C', 'O', 'O', 'K', 'I', 'E', '-', '1', 0,   16,
            0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15};
        require(record == std::string(reinterpret_cast<const char*>(expected.data()), expected.size()),
                "Xauthority binary format mismatch");
        rejects([&] { (void)xauthority_record("", 1, cookie); }, "Empty hostname accepted");
        rejects([&] { (void)xauthority_record(std::string("a\0b", 3), 1, cookie); }, "NUL hostname accepted");
        rejects([&] { (void)xauthority_record(std::string(256, 'a'), 1, cookie); },
                "Unbounded hostname accepted");
        rejects([&] { (void)xauthority_record("test", 65536, cookie); }, "Unbounded display accepted");
#if defined(__linux__) && !defined(__ANDROID__)
        if (argc == 2)
            live(argv[1], cookie);
#else
        (void)argc;
        (void)argv;
#endif
        std::cout << "Native Xauthority and X11 session checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
