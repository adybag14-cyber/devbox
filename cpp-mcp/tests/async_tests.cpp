#include "devbox/async.hpp"
#include "devbox/native.hpp"
#include "devbox/scoped_thread.hpp"
#include <future>
#include <iostream>
using namespace devbox;
namespace {
void scoped_thread_lifetime() {
    std::atomic_int stopped{0};
    auto cooperative = [&stopped](ThreadStopToken token) {
        while (!token.stop_requested())
            std::this_thread::sleep_for(Millis(1));
        ++stopped;
    };
    {
        ScopedThread first(cooperative), second(cooperative);
        first = std::move(second);
        if (stopped != 1 || second.joinable())
            throw Error("Thread move assignment did not stop and join the replaced worker");
        ScopedThread moved(std::move(first));
        if (first.joinable() || !moved.request_stop() || moved.request_stop())
            throw Error("Moved thread lost its single cooperative stop state");
    }
    if (stopped != 2)
        throw Error("Scope exit did not join the cooperative worker");
    bool joined = false;
    try {
        ScopedThread worker([value = std::make_unique<int>(7), &joined] {
            std::this_thread::sleep_for(Millis(10));
            joined = *value == 7;
        });
        throw Error("scope fixture");
    } catch (const Error&) {
    }
    if (!joined)
        throw Error("Exception unwinding did not join the worker with move-only state");
}
asio::awaitable<void> exercise(WorkPool& pool, const std::shared_ptr<std::string>& payload) {
    std::function<std::string()> callback = [text = *payload] { return text; };
    for (int i = 0; i < 20; ++i) {
        auto pending = pool.run([callback] { return callback(); });
        auto result = co_await std::move(pending);
        if (result != *payload)
            throw Error("Worker lost its owned callback payload");
    }
    bool rejected = false;
    try {
        auto pending = pool.run([copy = callback] {
            (void)copy();
            throw Error("worker fixture exception");
        });
        co_await std::move(pending);
    } catch (const Error& e) {
        rejected = std::string(e.what()) == "worker fixture exception";
    }
    if (!rejected)
        throw Error("Worker exception did not return through its coroutine");
    if (callback() != *payload)
        throw Error("Worker cleanup invalidated caller state");

    auto owned = std::make_shared<std::string>(8192, 's');
    const std::weak_ptr<std::string> after_timeout = owned;
    const auto started = Clock::now();
    auto slow = pool.run_until(
        [owned] {
            std::this_thread::sleep_for(Millis(250));
            return owned->size();
        },
        started + Millis(40));
    owned.reset();
    if (co_await std::move(slow))
        throw Error("Stalled worker ignored its caller deadline");
    if (Clock::now() - started > Millis(180) || after_timeout.expired())
        throw Error("Caller deadline waited for or destroyed the running worker payload");
    // A shared CI runner can delay the worker after its sleep. Observe actual
    // release with a bound instead of assuming a fixed scheduling interval.
    const auto release_deadline = Clock::now() + Millis(3000);
    while (!after_timeout.expired() && Clock::now() < release_deadline)
        co_await async_delay(Millis(10));
    if (!after_timeout.expired())
        throw Error("Late worker completion retained its payload");

    auto cancel = std::make_shared<Cancellation>();
    ScopedThread canceller([cancel] {
        std::this_thread::sleep_for(Millis(25));
        cancel->cancel();
    });
    bool stopped = false;
    try {
        auto pending = pool.run_until([] { std::this_thread::sleep_for(Millis(250)); },
                                      Clock::now() + Millis(1000), cancel);
        co_await std::move(pending);
    } catch (const Cancelled&) {
        stopped = true;
    }
    if (!stopped)
        throw Error("Bounded worker did not observe caller cancellation");
}
} // namespace
int main() {
    try {
        scoped_thread_lifetime();
        WorkPool pool(2, 8);
        asio::io_context io;
        auto payload = std::make_shared<std::string>(16384, 'x');
        const std::weak_ptr<std::string> observed = payload;
        auto result = asio::co_spawn(io, exercise(pool, payload), asio::use_future);
        io.run();
        result.get();
        payload.reset();
        if (!observed.expired())
            throw Error("Completed worker retained caller state");
        std::cout << "async ownership tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
