#pragma once
#include "common.hpp"
namespace devbox {
// Notifications are wake hints from an owner-only local directory. They never grant a lease;
// every waiter still checks queue order, capacity and the stored owner identity.
class SchedulerNotifications {
    struct State;
    std::unique_ptr<State> state_;

  public:
    explicit SchedulerNotifications(const fs::path& root);
    ~SchedulerNotifications();
    Cancel token() const;
    void signal() noexcept;
    Json snapshot() const;
};
std::shared_ptr<SchedulerNotifications> scheduler_notifications(const fs::path& root);
} // namespace devbox
