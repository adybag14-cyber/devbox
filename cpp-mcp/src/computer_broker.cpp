#include "devbox/computer_use.hpp"
#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <sddl.h>
#endif

namespace devbox {
#ifdef _WIN32
namespace {
constexpr std::uint32_t max_request = 64 * 1024, max_response = 16 * 1024 * 1024;
std::wstring pipe_path(std::string_view name) {
    if (name.empty() || name.size() > 100 || !std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        }))
        throw Error("COMPUTER_BROKER_CONFIG: use a local alphanumeric pipe name with optional hyphens");
    return L"\\\\.\\pipe\\" + wide(name);
}
std::vector<std::uint8_t> token_user(HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        throw Error("COMPUTER_BROKER_IDENTITY: cannot query peer token");
    NativeHandle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size || size > 65536)
        throw Error("COMPUTER_BROKER_IDENTITY: invalid token size");
    std::vector<std::uint8_t> bytes(size);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size))
        throw Error("COMPUTER_BROKER_IDENTITY: cannot read peer user");
    return bytes;
}
NativeHandle verify_peer(HANDLE pipe, bool server) {
    ULONG pid = 0;
    if (!(server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)))
        throw Error("COMPUTER_BROKER_IDENTITY: cannot identify pipe peer");
    NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
        throw Error("COMPUTER_BROKER_IDENTITY: peer exited");
    std::vector<wchar_t> path(32768);
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) ||
        _wcsicmp(path.data(), executable_path().c_str()) != 0)
        throw Error("COMPUTER_BROKER_IDENTITY: peer must run the same immutable executable");
    const auto own = token_user(GetCurrentProcess()), other = token_user(process.get());
    if (!EqualSid(reinterpret_cast<const TOKEN_USER*>(own.data())->User.Sid,
                  reinterpret_cast<const TOKEN_USER*>(other.data())->User.Sid))
        throw Error("COMPUTER_BROKER_IDENTITY: peer user differs");
    return process; // Keep the verified process object alive throughout this request.
}
void check(const Cancel& cancel, Clock::time_point deadline) {
    if (cancel)
        cancel->check();
    if (Clock::now() >= deadline)
        throw Error("COMPUTER_BROKER_TIMEOUT: desktop worker did not finish within 30 seconds");
}
void transfer(HANDLE pipe, std::span<std::uint8_t> bytes, bool writing, const Cancel& cancel,
              Clock::time_point deadline) {
    NativeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        throw Error("COMPUTER_BROKER_IO: cannot allocate I/O event");
    while (!bytes.empty()) {
        check(cancel, deadline);
        ResetEvent(event.get());
        OVERLAPPED pending{};
        pending.hEvent = event.get();
        DWORD count = 0;
        const auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536));
        const BOOL done = writing ? WriteFile(pipe, bytes.data(), size, &count, &pending)
                                  : ReadFile(pipe, bytes.data(), size, &count, &pending);
        if (!done && GetLastError() != ERROR_IO_PENDING)
            throw Error("COMPUTER_BROKER_DISCONNECTED: " + windows_error());
        if (!done) {
            ScopeExit drain([&] {
                CancelIoEx(pipe, &pending);
                GetOverlappedResult(pipe, &pending, &count, TRUE);
            });
            while (WaitForSingleObject(event.get(), 20) == WAIT_TIMEOUT)
                check(cancel, deadline);
            if (!GetOverlappedResult(pipe, &pending, &count, FALSE))
                throw Error("COMPUTER_BROKER_DISCONNECTED: " + windows_error());
            drain.disarm();
        }
        if (!count)
            throw Error("COMPUTER_BROKER_DISCONNECTED: empty transfer");
        bytes = bytes.subspan(count);
    }
}
void write_message(HANDLE pipe, const Json& message, std::uint32_t limit, const Cancel& cancel,
                   Clock::time_point deadline) {
    auto bytes = Json::to_cbor(message);
    if (bytes.empty() || bytes.size() > limit)
        throw Error("COMPUTER_BROKER_SIZE: message exceeds its bound");
    const auto size = static_cast<std::uint32_t>(bytes.size());
    std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(size >> 24),
                                       static_cast<std::uint8_t>(size >> 16),
                                       static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    transfer(pipe, header, true, cancel, deadline);
    transfer(pipe, bytes, true, cancel, deadline);
}
Json read_message(HANDLE pipe, std::uint32_t limit, const Cancel& cancel, Clock::time_point deadline) {
    std::array<std::uint8_t, 4> header{};
    transfer(pipe, header, false, cancel, deadline);
    std::uint32_t size = 0;
    for (auto byte : header)
        size = (size << 8) | byte;
    if (!size || size > limit)
        throw Error("COMPUTER_BROKER_SIZE: invalid message size");
    std::vector<std::uint8_t> bytes(size);
    transfer(pipe, bytes, false, cancel, deadline);
    return Json::from_cbor(bytes);
}
} // namespace
#endif

Json computer_broker_call(std::string_view name, std::string_view operation, const Json& arguments,
                          const Cancel& cancel) {
#ifdef _WIN32
    const auto path = pipe_path(name);
    if (cancel)
        cancel->check();
    NativeHandle pipe;
    const auto connect_until = Clock::now() + Millis(500);
    do {
        pipe.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                               nullptr));
        if (pipe || GetLastError() != ERROR_PIPE_BUSY)
            break;
        if (cancel)
            cancel->check();
        WaitNamedPipeW(path.c_str(), 20);
    } while (Clock::now() < connect_until);
    if (!pipe)
        throw Error("COMPUTER_BROKER_UNAVAILABLE: start the matching desktop worker in the logged-in "
                    "Windows session and retry observation");
    const auto peer = verify_peer(pipe.get(), false);
    Json reply;
    try {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        write_message(pipe.get(), Json{{"version", 1}, {"operation", operation}, {"arguments", arguments}},
                      max_request, cancel, deadline);
        const auto transport = std::make_shared<Cancellation>();
        {
            // Keep reading the response after cancellation: it acknowledges that held input was released.
            std::jthread cancellation_writer([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    if (cancel && cancel->cancelled()) {
                        try {
                            write_message(pipe.get(), Json{{"cancel", true}}, 64, {},
                                          Clock::now() + std::chrono::seconds(1));
                        } catch (...) {
                            transport->cancel();
                        }
                        return;
                    }
                    std::this_thread::sleep_for(Millis(10));
                }
            });
            reply = read_message(pipe.get(), max_response, transport, deadline);
        }
        write_message(pipe.get(), Json{{"received", true}}, 64, {}, deadline);
        if (cancel)
            cancel->check();
    } catch (const std::exception& error) {
        if (operation == "perform")
            throw Error("COMPUTER_INPUT_OUTCOME_UNKNOWN: desktop worker acknowledgement was lost; "
                        "observe again before deciding whether to retry. " +
                        std::string(error.what()));
        throw;
    }
    if (!json_bool(reply, "ok"))
        throw Error(json_string(reply, "error", "COMPUTER_BROKER_PROTOCOL: invalid reply"));
    return reply;
#else
    (void)name;
    (void)operation;
    (void)arguments;
    (void)cancel;
    throw Error("COMPUTER_UNSUPPORTED: desktop worker requires Windows");
#endif
}

int run_computer_broker(std::string_view name, const std::function<bool()>& stopping) {
#ifdef _WIN32
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !session)
        throw Error("COMPUTER_BROKER_SESSION: start the desktop worker with an Interactive task token");
    const auto path = pipe_path(name);
    const auto user = token_user(GetCurrentProcess());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &sid))
        throw Error("COMPUTER_BROKER_ACL: cannot resolve owner");
    ScopeExit free_sid([&] { LocalFree(sid); });
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr))
        throw Error("COMPUTER_BROKER_ACL: cannot create private pipe permissions");
    ScopeExit free_descriptor([&] { LocalFree(descriptor); });
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    NativeHandle pipe(CreateNamedPipeW(
        path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0,
        &security));
    if (!pipe)
        throw Error("COMPUTER_BROKER_BIND: local pipe is unavailable or already owned: " + windows_error());
    NativeHandle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connected)
        throw Error("COMPUTER_BROKER_IO: cannot allocate connection event");
    ComputerUse computer(false);
    Config config;
    config.platform = Platform::detect();
    config.runtime_mode = RuntimeMode::host;
    config.host_exec_enabled = true;
    ToolContract contract(config);
    std::cout << "Native CUA desktop worker ready in session " << session << '\n' << std::flush;
    while (!stopping()) {
        ResetEvent(connected.get());
        OVERLAPPED pending{};
        pending.hEvent = connected.get();
        const BOOL immediate = ConnectNamedPipe(pipe.get(), &pending);
        const auto error = immediate ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) {
            while (WaitForSingleObject(connected.get(), 20) == WAIT_TIMEOUT && !stopping()) {
            }
            DWORD count = 0;
            if (stopping())
                CancelIoEx(pipe.get(), &pending);
            const bool ok = GetOverlappedResult(pipe.get(), &pending, &count, TRUE) != FALSE;
            if (stopping())
                break;
            if (!ok)
                throw Error("COMPUTER_BROKER_CONNECT: " + windows_error());
        } else if (error != ERROR_PIPE_CONNECTED && error != ERROR_SUCCESS) {
            throw Error("COMPUTER_BROKER_CONNECT: " + windows_error(error));
        }
        ScopeExit disconnect([&] { DisconnectNamedPipe(pipe.get()); });
        const auto cancel = std::make_shared<Cancellation>();
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        try {
            const auto peer = verify_peer(pipe.get(), true);
            const auto request = read_message(pipe.get(), max_request, cancel, deadline);
            Json reply;
            {
                // A closed client pipe cancels native input and releases this action's held keys/buttons.
                std::jthread watcher([&](std::stop_token stop) {
                    while (!stop.stop_requested()) {
                        DWORD available = 0;
                        if (stopping() || Clock::now() >= deadline ||
                            WaitForSingleObject(peer.get(), 0) != WAIT_TIMEOUT ||
                            !PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
                            cancel->cancel();
                            return;
                        }
                        if (available) {
                            try {
                                const auto control = read_message(pipe.get(), 64, {}, deadline);
                                if (json_bool(control, "cancel"))
                                    cancel->cancel();
                            } catch (...) {
                                cancel->cancel();
                            }
                            return;
                        }
                        std::this_thread::sleep_for(Millis(10));
                    }
                });
                try {
                    if (json_uint(request, "version") != 1)
                        throw Error("COMPUTER_BROKER_PROTOCOL: unsupported protocol version");
                    const auto operation = json_string(request, "operation");
                    const auto& arguments = request.at("arguments");
                    if (operation == "windows") {
                        contract.arguments("host_computer_windows", arguments);
                        reply = Json{{"ok", true}, {"result", computer.windows(arguments, cancel)}};
                        reply["result"]["desktop_session"] = session;
                        reply["result"]["desktop_worker_pid"] = GetCurrentProcessId();
                    } else if (operation == "perform") {
                        contract.arguments("host_computer_use", arguments);
                        auto capture = computer.perform(arguments, cancel);
                        capture.metadata["desktop_session"] = session;
                        capture.metadata["desktop_worker_pid"] = GetCurrentProcessId();
                        reply = Json{{"ok", true},
                                     {"image", Json::binary(std::move(capture.image))},
                                     {"mime_type", capture.mime_type},
                                     {"metadata", capture.metadata}};
                    } else {
                        throw Error("COMPUTER_BROKER_PROTOCOL: unknown operation");
                    }
                } catch (const std::exception& failure) {
                    reply = Json{{"ok", false}, {"error", failure.what()}};
                }
            }
            write_message(pipe.get(), reply, max_response, {}, deadline);
            // Client acknowledgement prevents DisconnectNamedPipe discarding an unread response.
            const auto ack = read_message(pipe.get(), 64, {}, deadline);
            if (!json_bool(ack, "received"))
                throw Error("COMPUTER_BROKER_PROTOCOL: missing response acknowledgement");
        } catch (const std::exception&) {
            // No arguments, screenshots, text, or key values enter worker logs.
            // The authenticated MCP logs the caller-visible result; reconnects never replay input.
        }
    }
    return 0;
#else
    (void)name;
    (void)stopping;
    throw Error("COMPUTER_UNSUPPORTED: desktop worker requires Windows");
#endif
}
} // namespace devbox
