#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace devbox {
class ThreadStopToken {
    std::shared_ptr<std::atomic_bool> requested_;
    explicit ThreadStopToken(std::shared_ptr<std::atomic_bool> requested)
        : requested_(std::move(requested)) {}
    friend class ScopedThread;

  public:
    bool stop_requested() const noexcept {
        return requested_->load(std::memory_order_acquire);
    }
};

// Apple Xcode 16's libc++ does not provide std::jthread. These workers only
// require cooperative stop polling and scope-bound joining, with no callbacks.
// Keep that ownership contract on every supported C++20 library.
class ScopedThread {
    std::shared_ptr<std::atomic_bool> requested_;
    std::thread thread_;
    void stop_and_join() noexcept {
        request_stop();
        if (thread_.joinable())
            thread_.join();
    }

  public:
    ScopedThread() = default;
    template <class Fn>
        requires(!std::is_same_v<std::remove_cvref_t<Fn>, ScopedThread>)
    explicit ScopedThread(Fn&& function)
        : requested_(std::make_shared<std::atomic_bool>(false)),
          thread_([work = std::forward<Fn>(function), token = ThreadStopToken(requested_)]() mutable {
              if constexpr (std::is_invocable_v<decltype(work), ThreadStopToken>)
                  std::invoke(std::move(work), token);
              else
                  std::invoke(std::move(work));
          }) {}
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;
    ScopedThread(ScopedThread&&) noexcept = default;
    ScopedThread& operator=(ScopedThread&& other) noexcept {
        if (this != &other) {
            stop_and_join();
            requested_ = std::move(other.requested_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    ~ScopedThread() {
        stop_and_join();
    }
    bool request_stop() noexcept {
        return requested_ && !requested_->exchange(true, std::memory_order_acq_rel);
    }
    bool joinable() const noexcept {
        return thread_.joinable();
    }
    void join() {
        thread_.join();
    }
};
} // namespace devbox
