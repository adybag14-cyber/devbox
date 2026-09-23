// Deliberately independent of the broker library: this fixture must not import its GUI/profile APIs.
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#include <winsock2.h>
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        require(argc == 3, "probe arguments");
        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "inside token query");
        DWORD app = 0, lpac = 0, returned = 0;
        const auto is_app = GetTokenInformation(token, TokenIsAppContainer, &app, sizeof(app), &returned);
        const auto is_lpac =
            GetTokenInformation(token, TokenIsLessPrivilegedAppContainer, &lpac, sizeof(lpac), &returned);
        const auto lpac_error = GetLastError();
        CloseHandle(token);
        require(is_app && app == 1, "OS token is actually AppContainer");
        require((is_lpac && lpac == 1) || (!is_lpac && lpac_error == ERROR_INVALID_PARAMETER),
                "LPAC query must confirm the profile or explicitly report the unsupported information class");
        // Some Windows builds reject TokenIsLessPrivilegedAppContainer. Verify its actual
        // access semantics instead of weakening the assertion to ordinary AppContainer.
        std::ifstream all_apps("all-apps-only.txt", std::ios::binary);
        require(!all_apps, "LPAC cannot use the ordinary ALL APPLICATION PACKAGES ACE");
        PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY win32k{};
        require(GetProcessMitigationPolicy(GetCurrentProcess(), ProcessSystemCallDisablePolicy, &win32k,
                                           sizeof(win32k)) &&
                    win32k.DisallowWin32kSystemCalls,
                "desktop system calls disabled by OS mitigation");
        PROCESS_MITIGATION_CHILD_PROCESS_POLICY children{};
        require(GetProcessMitigationPolicy(GetCurrentProcess(), ProcessChildProcessPolicy, &children,
                                           sizeof(children)) &&
                    children.NoChildProcessCreation,
                "child process creation disabled by OS mitigation");
        wchar_t value[64];
        require(!GetEnvironmentVariableW(L"OPENAI_API_KEY", value, 64) &&
                    !GetEnvironmentVariableW(L"ISOLATION_TEST_SECRET", value, 64),
                "ambient credentials absent");
        std::ifstream outside(std::filesystem::path(argv[1]), std::ios::binary);
        require(!outside, "outside private file is unreadable");
        std::ofstream escaped("../escape.txt");
        require(!escaped, "outside workspace is not writable");
        {
            std::ofstream output("result.txt", std::ios::binary);
            output << "isolated-workspace-output";
            require(static_cast<bool>(output), "private workspace is writable");
        }
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock initialization");
        const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            const auto allocation_error = WSAGetLastError();
            WSACleanup();
            if (allocation_error != WSAEACCES)
                throw std::runtime_error("Unexpected socket denial: " + std::to_string(allocation_error));
            std::cout << "LPAC token, desktop and child restrictions, network and secret denial, workspace "
                         "access verified\n";
            return 0;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(std::stoi(argv[2])));
        const auto connected = ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        const auto error = WSAGetLastError();
        closesocket(socket);
        WSACleanup();
        require(connected == SOCKET_ERROR && error == WSAEACCES, "network access denied by OS policy");
        std::cout << "LPAC token, desktop and child restrictions, network and secret denial, workspace "
                     "access verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
#elif defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("probe arguments");
        if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 || ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2)
            throw std::runtime_error("kernel privilege and seccomp policy required");
        if (std::getenv("OPENAI_API_KEY") || std::getenv("ISOLATION_TEST_SECRET"))
            throw std::runtime_error("ambient credential leaked");
        if (std::ifstream(argv[1]))
            throw std::runtime_error("outside private file readable");
        if (::socket(AF_INET, SOCK_STREAM, 0) != -1 || errno != EACCES)
            throw std::runtime_error("network socket policy did not deny access");
        const auto child = ::fork();
        if (child == 0)
            ::_exit(41);
        if (child >= 0) {
            int status = 0;
            ::waitpid(child, &status, 0);
            throw std::runtime_error("child process was permitted");
        }
        if (errno != EPERM)
            throw std::runtime_error("unexpected child-process denial");
        bool ran = false;
        std::thread thread([&] { ran = true; });
        thread.join();
        if (!ran)
            throw std::runtime_error("ordinary worker thread unavailable");
        {
            std::ofstream output("result.txt");
            output << "isolated-workspace-output";
            if (!output)
                throw std::runtime_error("workspace not writable");
        }
        if (std::ofstream("../escape.txt"))
            throw std::runtime_error("namespace root should be readonly");
        for (int fd = 3; fd < 256; ++fd)
            if (::fcntl(fd, F_GETFD) != -1 || errno != EBADF)
                throw std::runtime_error("unexpected inherited descriptor");
        std::cout << "Linux namespaces, seccomp, network and child denial, secret denial and "
                     "thread/workspace access verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}
#else
int main() {
    return 77;
}
#endif
