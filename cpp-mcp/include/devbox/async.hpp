#pragma once
#include "common.hpp"
#include <boost/asio.hpp>
#include <deque>
#include <thread>
#include <type_traits>
#include <variant>

namespace devbox {
namespace asio = boost::asio;
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

  public:
    explicit WorkPool(std::size_t workers, std::size_t capacity = 128);
    ~WorkPool();
    WorkPool(const WorkPool&) = delete;
    WorkPool& operator=(const WorkPool&) = delete;
    std::size_t queued() const;
    template <class Fn>
    asio::awaitable<std::conditional_t<std::is_void_v<std::invoke_result_t<Fn>>, std::monostate,
                                       std::invoke_result_t<Fn>>>
    run(Fn fn, Cancel cancel = {}) {
        using T = std::conditional_t<std::is_void_v<std::invoke_result_t<Fn>>, std::monostate,
                                     std::invoke_result_t<Fn>>;
        auto initiate = [this, work = std::make_shared<Fn>(std::move(fn)),
                         cancel = std::move(cancel)](auto handler) mutable {
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
};
asio::awaitable<void> async_delay(Millis delay, const Cancel& cancel = {});
} // namespace devbox
