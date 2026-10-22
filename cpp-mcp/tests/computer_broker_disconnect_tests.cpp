#include "devbox/computer_use.hpp"
#include "devbox/native.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>

using namespace devbox;
#ifdef _WIN32
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}

void disconnected_before_accept(bool stop_during_gap) {
    const auto name = "Devbox-Disconnect-Test-" + uuid();
    const auto path = L"\\\\.\\pipe\\" + wide(name);
    std::promise<void> created, release, retrying;
    auto ready = created.get_future();
    auto released = release.get_future();
    auto retried = retrying.get_future();
    std::atomic_bool stop{false}, finished{false};
    std::string error;
    int result = -1;
    std::jthread worker([&] {
        unsigned checks = 0;
        try {
            result = run_computer_broker(name, [&] {
                const bool observed = stop.load();
                if (++checks == 1) {
                    // The pipe now exists but ConnectNamedPipe has not run.
                    // Force a client to connect and close in this exact gap.
                    created.set_value();
                    if (released.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                        throw Error("fixture did not release initial accept gap");
                } else if (checks == 2) {
                    retrying.set_value();
                }
                return observed;
            });
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        finished = true;
    });
    bool gap_released = false;
    ScopeExit cleanup([&] {
        stop = true;
        if (!gap_released)
            release.set_value();
        worker.join();
    });
    require(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "owned broker did not reach the accept gap");
    {
        NativeHandle client(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED, nullptr));
        require(static_cast<bool>(client), "fixture client did not connect before accept");
    }
    if (stop_during_gap)
        stop = true;
    release.set_value();
    gap_released = true;
    if (!stop_during_gap) {
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (retried.wait_for(Millis(10)) != std::future_status::ready && !finished) {
            if (Clock::now() >= until)
                throw Error("broker did not recover its listener after early disconnect");
        }
        if (finished) {
            worker.join();
            cleanup.disarm();
            throw Error("broker exited after early disconnect: " + error);
        }
        bool rejected = false;
        try {
            // An authenticated protocol error exercises a fresh connection and
            // acknowledgement without enumerating windows or injecting input.
            (void)computer_broker_call(name, "invalid-fixture-operation", Json::object(), {});
        } catch (const Error& failure) {
            rejected = std::string_view(failure.what()).starts_with("COMPUTER_BROKER_PROTOCOL:");
        }
        require(rejected, "fresh client did not reach the recovered protocol handler");
        stop = true;
    }
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (!finished) {
        if (Clock::now() >= until)
            throw Error("owned broker did not stop within its fixture bound");
        std::this_thread::sleep_for(Millis(10));
    }
    worker.join();
    cleanup.disarm();
    if (!error.empty())
        throw Error("broker shutdown after early disconnect: " + error);
    require(result == 0, "broker did not return successful shutdown");
}
} // namespace
#endif

int main() {
    try {
#ifdef _WIN32
        disconnected_before_accept(false);
        disconnected_before_accept(true);
        std::cout
            << "early client disconnect, listener recovery and concurrent stop passed; no desktop input\n";
#endif
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
