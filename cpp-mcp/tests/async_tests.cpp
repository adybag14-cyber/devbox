#include "devbox/async.hpp"
#include "devbox/native.hpp"
#include "devbox/scoped_thread.hpp"
#include <future>
#include <iostream>
using namespace devbox;
namespace {
asio::awaitable<int> blocked_work(WorkPool& pool, std::atomic_int& active, std::shared_future<void> gate) {
    auto pending = pool.run([&active, gate] {
        ++active;
        gate.wait();
        --active;
        return 1;
    });
    co_return co_await std::move(pending);
}
void bounded_worker_growth() {
    WorkPool pool(2, 3);
    asio::io_context io;
    auto keep_running = asio::make_work_guard(io);
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic_int active{0};
    std::vector<std::future<int>> results;
    ScopedThread executor([&] { io.run(); });
    bool released = false;
    ScopeExit stop([&] {
        if (!released)
            release.set_value();
        keep_running.reset();
    });
    for (int i = 0; i < 2; ++i)
        results.push_back(asio::co_spawn(io, blocked_work(pool, active, gate), asio::use_future));
    const auto started = Clock::now();
    while (active != 2 && Clock::now() - started < Millis(2000))
        std::this_thread::sleep_for(Millis(1));
    if (active != 2)
        throw Error("Worker pool did not grow to serve concurrent operations");
    for (int i = 0; i < 3; ++i)
        results.push_back(asio::co_spawn(io, blocked_work(pool, active, gate), asio::use_future));
    const auto queued_at = Clock::now();
    while (pool.queued() != 3 && Clock::now() - queued_at < Millis(2000))
        std::this_thread::sleep_for(Millis(1));
    if (pool.queued() != 3 || active != 2)
        throw Error("Worker pool exceeded configured concurrency or lost queued work");
    auto overflow = asio::co_spawn(io, blocked_work(pool, active, gate), asio::use_future);
    if (overflow.wait_for(Millis(2000)) != std::future_status::ready)
        throw Error("A full worker queue did not reject promptly");
    bool rejected = false;
    try {
        (void)overflow.get();
    } catch (const Error& error) {
        rejected =
            std::string_view(error.what()).find("Bounded worker queue is full") != std::string_view::npos;
    }
    if (!rejected)
        throw Error("Worker pool accepted work beyond its queue capacity");
    release.set_value();
    released = true;
    for (auto& result : results)
        if (result.get() != 1)
            throw Error("Worker pool failed to drain accepted work");
}
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
        bounded_worker_growth();
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
