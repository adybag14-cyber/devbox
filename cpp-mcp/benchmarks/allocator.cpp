#include "devbox/allocator.hpp"
#include "devbox/common.hpp"
#include <algorithm>
#include <barrier>
#include <iostream>
#include <new>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif
using namespace devbox;
namespace {
double cpu_ms() {
#ifdef _WIN32
    FILETIME a{}, b{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &a, &b, &kernel, &user))
        throw Error("CPU clock unavailable");
    const auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage))
        throw Error("CPU clock unavailable");
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
#endif
}
void precision() {
    // Neither observation allocates; check over-alignment and a sized cross-thread free.
    std::barrier ready(2), done(2);
    void* pointer = nullptr;
    std::thread worker([&] {
        ready.arrive_and_wait();
        ready.arrive_and_wait();
        ::operator delete(pointer, std::size_t(257), std::align_val_t(256));
        done.arrive_and_wait();
    });
    ready.arrive_and_wait();
    const auto before = allocator_counters();
    pointer = ::operator new(257, std::align_val_t(256));
    const auto held = allocator_counters();
    const bool aligned = reinterpret_cast<std::uintptr_t>(pointer) % 256 == 0;
    ready.arrive_and_wait();
    done.arrive_and_wait();
    // Thread retirement can free its own bookkeeping. Check precise allocation
    // deltas before release and whole-process accounting again after join.
    worker.join();
    const auto after = allocator_counters();
    if (!aligned || held.current != before.current + 257 || held.calls != before.calls + 1 ||
        held.allocated != before.allocated + 257 || after.current != after.allocated - after.freed ||
        after.active != after.calls - after.deletes || after.peak < held.current)
        throw Error("Allocator accounting/overalignment/cross-thread free regression");
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto workers = argc > 1 ? std::stoul(argv[1]) : 8;
        const auto count = argc > 2 ? std::stoul(argv[2]) : 1000;
        if (!workers || workers > 64 || count < 1000 || count > 10000)
            throw Error("Usage: allocator-bench THREADS(1..64) SAMPLES(1000..10000)");
        precision();
        std::vector<std::vector<double>> samples(workers, std::vector<double>(count));
        std::vector<std::thread> threads;
        threads.reserve(workers);
        std::barrier start(static_cast<std::ptrdiff_t>(workers + 1)),
            stop(static_cast<std::ptrdiff_t>(workers + 1));
        std::atomic<std::uint64_t> verified{0};
        for (std::size_t t = 0; t < workers; ++t)
            threads.emplace_back([&, t] {
                start.arrive_and_wait();
                std::uint64_t bytes = 0;
                for (std::size_t s = 0; s < count; ++s) {
                    const auto begun = Clock::now();
                    for (std::size_t i = 0; i < 128; ++i) {
                        const std::string payload(32 + ((s + i) % 8) * 128, 'x');
                        Json value{{"id", s}, {"name", "fixed_research_document"}, {"text", payload}};
                        const auto encoded = value.dump();
                        bytes += encoded.size();
                    }
                    samples[t][s] = std::chrono::duration<double, std::micro>(Clock::now() - begun).count();
                }
                verified.fetch_add(bytes);
                stop.arrive_and_wait();
            });
        const auto before = cpu_ms();
        const auto began = Clock::now();
        start.arrive_and_wait();
        stop.arrive_and_wait();
        const auto wall = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        const auto cpu = cpu_ms() - before;
        for (auto& thread : threads)
            thread.join();
        std::vector<double> all;
        all.reserve(workers * count);
        for (const auto& row : samples)
            all.insert(all.end(), row.begin(), row.end());
        std::sort(all.begin(), all.end());
        const auto counters = allocator_counters();
        if (counters.current != counters.allocated - counters.freed ||
            counters.active != counters.calls - counters.deletes)
            throw Error("Quiescent counters lost allocation events");
        std::cout << Json{{"mode", DEVBOX_ALLOCATOR_SHARDED ? "sharded" : "global"},
                          {"workers", workers},
                          {"samples", all.size()},
                          {"operations_per_sample", 128},
                          {"cpu_ms", cpu},
                          {"wall_ms", wall},
                          {"verified_bytes", verified.load()},
                          {"p50_us", all[all.size() / 2]},
                          {"p95_us", all[all.size() * 95 / 100]},
                          {"p99_us", all[all.size() * 99 / 100]},
                          {"precision_checks", true}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
