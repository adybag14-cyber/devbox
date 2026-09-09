#include "devbox/common.hpp"
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
            allocated.fetch_add(size, std::memory_order_relaxed);
            active.fetch_add(1, std::memory_order_relaxed);
            calls.fetch_add(1, std::memory_order_relaxed);
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
    freed.fetch_add(header->size, std::memory_order_relaxed);
    active.fetch_sub(1, std::memory_order_relaxed);
    deletes.fetch_add(1, std::memory_order_relaxed);
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
Json allocator_snapshot() {
#if defined(DEVBOX_SANITIZERS)
    return Json{{"backend", "sanitizer"}, {"available", false}};
#else
    // Load counters before constructing JSON, which itself allocates memory.
    const auto c = current.load(), p = peak.load(), a = allocated.load(), f = freed.load(), n = active.load(),
               ac = calls.load(), dc = deletes.load();
    return Json{{"backend", "cpp-global-new"},   {"currentRequestedBytes", c}, {"peakRequestedBytes", p},
                {"cumulativeAllocatedBytes", a}, {"cumulativeFreedBytes", f},  {"activeAllocations", n},
                {"allocationCalls", ac},         {"deallocationCalls", dc},    {"reallocationCalls", 0}};
#endif
}
} // namespace devbox
