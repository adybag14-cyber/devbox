#include "devbox/async.hpp"
namespace devbox {
WorkPool::WorkPool(std::size_t workers, std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {
    for (std::size_t i = 0; i < std::max<std::size_t>(1, workers); ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> next;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                    if (queue_.empty() && stopping_)
                        return;
                    next = std::move(queue_.front());
                    queue_.pop_front();
                }
                next();
            }
        });
    }
}
WorkPool::~WorkPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    for (auto& worker : workers_)
        worker.join();
}
bool WorkPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= capacity_)
            return false;
        queue_.push_back(std::move(task));
    }
    changed_.notify_one();
    return true;
}
std::size_t WorkPool::queued() const {
    auto& self = const_cast<WorkPool&>(*this);
    std::lock_guard lock(self.mutex_);
    return self.queue_.size();
}
asio::awaitable<void> async_delay(Millis delay, Cancel cancel) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    const auto deadline = Clock::now() + std::max(Millis(0), delay);
    while (Clock::now() < deadline) {
        if (cancel)
            cancel->check();
        timer.expires_at(std::min(deadline, Clock::now() + Millis(50)));
        co_await timer.async_wait(asio::use_awaitable);
    }
    if (cancel)
        cancel->check();
}
} // namespace devbox
