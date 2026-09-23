#include "devbox/posix_process.hpp"
#ifndef _WIN32
#include "devbox/scoped_thread.hpp"
#include <array>
#include <cerrno>
#include <sys/wait.h>
namespace devbox {
namespace {
class ChildReaper {
    std::mutex mutex_;
    std::condition_variable wake_;
    std::array<pid_t, 4096> children_{}; // zero=free, -1=reserved, positive=owned unreaped child
    std::size_t deferred_ = 0;
    ScopedThread thread_;

  public:
    ChildReaper()
        : thread_([this](ThreadStopToken stop) {
              std::unique_lock lock(mutex_);
              while (!stop.stop_requested()) {
                  for (auto& pid : children_)
                      if (pid > 0) {
                          int status = 0;
                          const auto result = ::waitpid(pid, &status, WNOHANG);
                          if (result == pid || (result < 0 && errno == ECHILD)) {
                              pid = 0;
                              --deferred_;
                          }
                      }
                  if (deferred_)
                      wake_.wait_for(lock, Millis(50));
                  else
                      wake_.wait(lock, [&] { return deferred_ || stop.stop_requested(); });
              }
          }) {}
    ~ChildReaper() {
        thread_.request_stop();
        wake_.notify_all();
        thread_.join();
    }
    std::size_t reserve() {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < children_.size(); ++i)
            if (!children_[i]) {
                children_[i] = -1;
                return i;
            }
        throw Error("PROCESS_REAPER_CAPACITY: outstanding child cleanup prevents further launch");
    }
    void release(std::size_t slot) noexcept {
        std::lock_guard lock(mutex_);
        if (slot < children_.size() && children_[slot] == -1)
            children_[slot] = 0;
    }
    void defer(std::size_t slot, pid_t child) noexcept {
        std::lock_guard lock(mutex_);
        if (slot < children_.size() && children_[slot] == -1 && child > 0) {
            children_[slot] = child;
            ++deferred_;
            wake_.notify_one();
        }
    }
};
ChildReaper& reaper() {
    static ChildReaper value;
    return value;
}
} // namespace
std::size_t reserve_posix_reap_slot() {
    return reaper().reserve();
}
void release_posix_reap_slot(std::size_t slot) noexcept {
    reaper().release(slot);
}
void defer_posix_reap(std::size_t slot, pid_t child) noexcept {
    reaper().defer(slot, child);
}
} // namespace devbox
#endif
