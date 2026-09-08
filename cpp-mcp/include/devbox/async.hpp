#pragma once
#include "common.hpp"
#include <boost/asio.hpp>
#include <deque>
#include <future>
#include <thread>
#include <type_traits>
#include <variant>

namespace devbox {
namespace asio = boost::asio;
asio::awaitable<void> async_delay(Millis delay, Cancel cancel = {});
class WorkPool {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::size_t capacity_;
    bool stopping_ = false;
    bool enqueue(std::function<void()> task);
    template <class T> struct Result {
        std::exception_ptr error;
        std::optional<T> value;
    };

    template <class Fn>
    asio::awaitable<std::optional<std::conditional_t<std::is_void_v<std::invoke_result_t<Fn>>, std::monostate,
                                                     std::invoke_result_t<Fn>>>>
    run_until_owned(std::shared_ptr<Fn> work, Clock::time_point deadline, Cancel cancel) {
        using T = std::conditional_t<std::is_void_v<std::invoke_result_t<Fn>>, std::monostate,
                                     std::invoke_result_t<Fn>>;
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        // This completion has no executor or coroutine reference. A slow OS
        // filesystem operation may finish after the caller's deadline safely.
        auto task = [promise, work, deadline, cancel] {
            try {
                if (cancel)
                    cancel->check();
                if (Clock::now() >= deadline)
                    throw Error("Worker operation expired before leaving its bounded queue");
                if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
                    (*work)();
                    promise->set_value(std::monostate{});
                } else
                    promise->set_value((*work)());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        };
        if (!enqueue(std::move(task)))
            throw Error("Bounded worker queue is full; retry shortly.");
        while (future.wait_for(Millis(0)) != std::future_status::ready) {
            if (cancel)
                cancel->check();
            const auto now = Clock::now();
            if (now >= deadline)
                co_return std::nullopt;
            co_await async_delay(std::min(Millis(10), std::chrono::duration_cast<Millis>(deadline - now)),
                                 cancel);
        }
        if (cancel)
            cancel->check();
        if (Clock::now() >= deadline)
            co_return std::nullopt;
        co_return std::optional<T>(future.get());
    }

    template <class Fn>
    asio::awaitable<std::conditional_t<std::is_void_v<std::invoke_result_t<Fn>>, std::monostate,
                                       std::invoke_result_t<Fn>>>
    run_owned(std::shared_ptr<Fn> work, Cancel cancel) {
        using T = std::conditional_t<std::is_void_v<std::invoke_result_t<Fn>>, std::monostate,
                                     std::invoke_result_t<Fn>>;
        auto initiate = [this, work = std::move(work), cancel = std::move(cancel)](auto handler) mutable {
            auto executor = asio::get_associated_executor(handler);
            auto guard =
                std::make_shared<decltype(asio::make_work_guard(executor))>(asio::make_work_guard(executor));
            auto receiver = std::make_shared<decltype(handler)>(std::move(handler));
            auto complete = [executor, receiver, guard](Result<T> result) mutable {
                asio::post(executor, [receiver, guard, result = std::move(result)]() mutable {
                    (*receiver)(std::move(result));
                });
            };
            auto task = [work, cancel, complete]() mutable {
                Result<T> result;
                try {
                    if (cancel)
                        cancel->check();
                    if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
                        (*work)();
                        result.value.emplace();
                    } else
                        result.value.emplace((*work)());
                } catch (...) {
                    result.error = std::current_exception();
                }
                complete(std::move(result));
            };
            if (!enqueue(std::move(task))) {
                Result<T> result;
                result.error = std::make_exception_ptr(Error("Bounded worker queue is full; retry shortly."));
                complete(std::move(result));
            }
        };
        auto result = co_await asio::async_initiate<decltype(asio::use_awaitable), void(Result<T>)>(
            std::move(initiate), asio::use_awaitable);
        if (result.error)
            std::rethrow_exception(result.error);
        co_return std::move(*result.value);
    }

  public:
    explicit WorkPool(std::size_t workers, std::size_t capacity = 128);
    ~WorkPool();
    WorkPool(const WorkPool&) = delete;
    WorkPool& operator=(const WorkPool&) = delete;
    std::size_t queued() const;
    template <class Fn> auto run(Fn fn, Cancel cancel = {}) {
        // Transfer the callable before constructing a suspended coroutine. Callers
        // bind this returned awaitable to a local, then co_await it in a separate
        // expression: GCC PR101243 can destroy temporary lambda captures twice
        // when lambda construction and co_await appear in one expression.
        return run_owned(std::make_shared<Fn>(std::move(fn)), std::move(cancel));
    }
    template <class Fn> auto run_until(Fn fn, Clock::time_point deadline, Cancel cancel = {}) {
        // The callable must own its state because it can outlive the awaiter.
        // As with run(), bind the awaitable before co_await on GCC 12.
        return run_until_owned(std::make_shared<Fn>(std::move(fn)), deadline, std::move(cancel));
    }
};
} // namespace devbox
