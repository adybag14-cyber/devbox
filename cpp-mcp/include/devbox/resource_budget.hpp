#pragma once
#include "common.hpp"
namespace devbox {
class ResourceExhausted : public Error {
  public:
    using Error::Error;
};
class ByteBudget {
    struct State {
        const std::size_t capacity;
        std::atomic_size_t used{0}, peak{0}, rejected{0};
        explicit State(std::size_t bytes) : capacity(bytes) {}
    };
    std::shared_ptr<State> state_;

  public:
    class Reservation {
        friend class ByteBudget;
        std::shared_ptr<State> state_;
        std::size_t bytes_;
        Reservation(std::shared_ptr<State> state, std::size_t bytes)
            : state_(std::move(state)), bytes_(bytes) {}

      public:
        ~Reservation() {
            state_->used.fetch_sub(bytes_, std::memory_order_relaxed);
        }
        Reservation(const Reservation&) = delete;
    };
    using Lease = std::shared_ptr<Reservation>;
    explicit ByteBudget(std::size_t capacity) : state_(std::make_shared<State>(capacity)) {}
    Lease acquire(std::size_t bytes) {
        auto used = state_->used.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > state_->capacity || used > state_->capacity - bytes) {
                ++state_->rejected;
                throw ResourceExhausted(
                    "RESOURCE_BYTE_BUDGET: server capacity is occupied; retry after existing work completes");
            }
            if (state_->used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
                break;
        }
        auto peak = state_->peak.load(std::memory_order_relaxed);
        while (peak < used + bytes &&
               !state_->peak.compare_exchange_weak(peak, used + bytes, std::memory_order_relaxed)) {
        }
        Reservation* reservation;
        try {
            reservation = new Reservation(state_, bytes);
        } catch (...) {
            state_->used.fetch_sub(bytes, std::memory_order_relaxed);
            throw;
        }
        // shared_ptr deletes the reservation if allocating its control block fails.
        return std::shared_ptr<Reservation>(reservation);
    }
    Json snapshot() const {
        return Json{{"capacity_bytes", state_->capacity},
                    {"used_bytes", state_->used.load()},
                    {"peak_bytes", state_->peak.load()},
                    {"rejected", state_->rejected.load()}};
    }
};
// Conservative DOM, encoding and temporary-string charge. Returns limit+1 without walking more
// nodes once the budget is exceeded; never performs payload serialization on the I/O thread.
std::size_t json_memory_charge(const Json& value, std::size_t limit, unsigned depth = 0);
std::string bounded_json_dump(const Json& value, std::size_t maximum, const Cancel& cancel = {});
} // namespace devbox
