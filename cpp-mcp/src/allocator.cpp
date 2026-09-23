#include "devbox/allocator.hpp"
#include "devbox/common.hpp"
#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
// All standard C++ new/delete forms share requested-byte accounting. C libraries
// using malloc directly are deliberately outside this metric; RSS covers them.
#if !defined(DEVBOX_SANITIZERS)
namespace {
struct Header {
    void* base;
    std::size_t size;
};
std::atomic<std::uint64_t> current{0}, peak{0}, allocated{0}, freed{0}, active{0}, calls{0}, deletes{0};
#if DEVBOX_ALLOCATOR_SHARDED
struct alignas(64) CounterShard {
    std::atomic<std::uint64_t> allocated{0}, freed{0}, calls{0}, deletes{0};
};
std::array<CounterShard, 64> shards{};
std::atomic<unsigned> next_shard{0};
CounterShard& local_counters() noexcept {
    // Slots never retire: cross-thread frees and exited threads remain counted.
    // Collisions share atomics and preserve precision even beyond 64 threads.
    thread_local const auto index = next_shard.fetch_add(1, std::memory_order_relaxed) % shards.size();
    return shards[index];
}
#endif
void* allocate(std::size_t size, std::size_t alignment) {
    if (alignment < alignof(Header))
        alignment = alignof(Header);
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(Header) - alignment)
        throw std::bad_alloc();
    for (;;) {
        if (void* base = std::malloc(size + sizeof(Header) + alignment)) {
            auto address =
                (reinterpret_cast<std::uintptr_t>(base) + sizeof(Header) + alignment - 1) & ~(alignment - 1);
            auto* header = reinterpret_cast<Header*>(address) - 1;
            header->base = base;
            header->size = size;
            const auto now = current.fetch_add(size, std::memory_order_relaxed) + size;
            auto prior = peak.load(std::memory_order_relaxed);
            while (prior < now && !peak.compare_exchange_weak(prior, now, std::memory_order_relaxed)) {
            }
            active.fetch_add(1, std::memory_order_relaxed);
#if DEVBOX_ALLOCATOR_SHARDED
            auto& counters = local_counters();
            counters.allocated.fetch_add(size, std::memory_order_relaxed);
            counters.calls.fetch_add(1, std::memory_order_relaxed);
#else
            allocated.fetch_add(size, std::memory_order_relaxed);
            calls.fetch_add(1, std::memory_order_relaxed);
#endif
            return reinterpret_cast<void*>(address);
        }
        auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}
void release(void* ptr) noexcept {
    if (!ptr)
        return;
    auto* header = static_cast<Header*>(ptr) - 1;
    current.fetch_sub(header->size, std::memory_order_relaxed);
    active.fetch_sub(1, std::memory_order_relaxed);
#if DEVBOX_ALLOCATOR_SHARDED
    auto& counters = local_counters();
    counters.freed.fetch_add(header->size, std::memory_order_relaxed);
    counters.deletes.fetch_add(1, std::memory_order_relaxed);
#else
    freed.fetch_add(header->size, std::memory_order_relaxed);
    deletes.fetch_add(1, std::memory_order_relaxed);
#endif
    std::free(header->base);
}
} // namespace
void* operator new(std::size_t n) {
    return allocate(n, alignof(std::max_align_t));
}
void* operator new[](std::size_t n) {
    return allocate(n, alignof(std::max_align_t));
}
void* operator new(std::size_t n, std::align_val_t a) {
    return allocate(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return allocate(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n, a);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](n, a);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept {
    release(p);
}
void operator delete[](void* p) noexcept {
    release(p);
}
void operator delete(void* p, std::size_t) noexcept {
    release(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    release(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    release(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    release(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    release(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    release(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    release(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    release(p);
}
#endif
namespace devbox {
AllocatorCounters allocator_counters() noexcept {
#if defined(DEVBOX_SANITIZERS)
    return {};
#else
    AllocatorCounters result{current.load(std::memory_order_relaxed),
                             peak.load(std::memory_order_relaxed),
                             0,
                             0,
                             active.load(std::memory_order_relaxed),
                             0,
                             0};
#if DEVBOX_ALLOCATOR_SHARDED
    for (const auto& shard : shards) {
        result.allocated += shard.allocated.load(std::memory_order_relaxed);
        result.freed += shard.freed.load(std::memory_order_relaxed);
        result.calls += shard.calls.load(std::memory_order_relaxed);
        result.deletes += shard.deletes.load(std::memory_order_relaxed);
    }
#else
    result.allocated = allocated.load(std::memory_order_relaxed);
    result.freed = freed.load(std::memory_order_relaxed);
    result.calls = calls.load(std::memory_order_relaxed);
    result.deletes = deletes.load(std::memory_order_relaxed);
#endif
    return result;
#endif
}
Json allocator_snapshot() {
#if defined(DEVBOX_SANITIZERS)
    return Json{{"backend", "sanitizer"}, {"available", false}};
#else
    // Load counters before constructing JSON, which itself allocates memory.
    const auto c = allocator_counters();
    return Json{{"backend", DEVBOX_ALLOCATOR_SHARDED ? "cpp-global-new-sharded" : "cpp-global-new"},
                {"currentRequestedBytes", c.current},
                {"peakRequestedBytes", c.peak},
                {"cumulativeAllocatedBytes", c.allocated},
                {"cumulativeFreedBytes", c.freed},
                {"activeAllocations", c.active},
                {"allocationCalls", c.calls},
                {"deallocationCalls", c.deletes},
                {"reallocationCalls", 0}};
#endif
}
} // namespace devbox
