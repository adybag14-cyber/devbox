#include "devbox/async.hpp"
namespace devbox {
WorkPool::WorkPool(std::size_t workers, std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)), worker_limit_(std::max<std::size_t>(1, workers)) {
    workers_.reserve(worker_limit_);
}
void WorkPool::worker() {
    while (true) {
        std::function<void()> next;
        {
            std::unique_lock lock(mutex_);
            ++idle_workers_;
            changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            --idle_workers_;
            if (queue_.empty() && stopping_)
                return;
            next = std::move(queue_.front());
            queue_.pop_front();
        }
        next();
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
        // Retain the configured concurrency and queue bounds, but start OS
        // threads only when admitted work exceeds the currently idle workers.
        if (queue_.size() > idle_workers_ && workers_.size() < worker_limit_) {
            try {
                workers_.emplace_back([this] { worker(); });
            } catch (...) {
                if (workers_.empty()) {
                    queue_.pop_back();
                    throw;
                }
                // Existing workers can still drain the accepted bounded queue.
            }
        }
    }
    changed_.notify_one();
    return true;
}
std::size_t WorkPool::queued() const {
    auto& self = const_cast<WorkPool&>(*this);
    std::lock_guard lock(self.mutex_);
    return self.queue_.size();
}
namespace {
asio::awaitable<void> cancellable_delay(Clock::time_point deadline, Cancel cancel) {
    const auto executor = co_await asio::this_coro::executor;
    auto timer = std::make_shared<asio::steady_timer>(executor, deadline);
    const std::weak_ptr<asio::steady_timer> observed = timer;
    auto subscription = cancel->subscribe([executor, observed] {
        // Always post: the strand must arm the wait before cancellation touches
        // the timer, including cancellation that predates subscription.
        asio::post(executor, [observed] {
            if (const auto timer = observed.lock())
                timer->cancel();
        });
    });
    boost::system::error_code error;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
    subscription.reset();
    cancel->check();
    if (error)
        throw boost::system::system_error(error);
}
} // namespace
asio::awaitable<void> async_delay(Millis delay, Cancel cancel) {
    const auto deadline = Clock::now() + std::max(Millis(0), delay);
    if (cancel)
        cancel->check();
    if (delay <= Millis(0))
        co_return;
    const auto executor = co_await asio::this_coro::executor;
    if (cancel) {
        // Foreign threads can cancel tokens. A private strand serializes timer
        // creation, arming, cancellation and destruction on multithreaded I/O.
        co_await asio::co_spawn(asio::make_strand(executor), cancellable_delay(deadline, std::move(cancel)),
                                asio::use_awaitable);
    } else {
        asio::steady_timer timer(executor, deadline);
        co_await timer.async_wait(asio::use_awaitable);
    }
}
} // namespace devbox
