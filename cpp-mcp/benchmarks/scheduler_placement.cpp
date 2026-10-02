#include "devbox/contract.hpp"
#include "devbox/scheduler.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw Error("Supply a NEW absolute fixture directory");
        const auto root = path_from_utf8(argv[1]);
        if (!root.is_absolute() || !fs::create_directory(root))
            throw Error("Fixture directory must be new and absolute");
        std::cout << Json{{"build", build_snapshot()}, {"fixtureRoot", path_text(root)}}.dump() << '\n';
        for (const auto holder_class : {ResourceClass::light, ResourceClass::heavy}) {
            SchedulerConfig config;
            config.root = root / resource_name(holder_class);
            config.max_concurrent = 10;
            config.reserved_interactive = 1;
            config.heavy_capacity = 5;
            config.io_heavy_capacity = 2;
            config.queue_timeout = Millis(2000);
            ExecutionScheduler scheduler(config);
            std::vector<double> samples;
            std::size_t acquired = 0;
            for (int i = 0; i < 22; ++i) {
                auto held = scheduler.acquire({ExecutionKind::background,
                                               holder_class,
                                               holder_class == ResourceClass::heavy ? 2u : 1u,
                                               "bounded unrelated holder",
                                               {}});
                auto release = std::async(std::launch::async, [lease = std::move(held)]() mutable {
                    std::this_thread::sleep_for(Millis(80));
                    lease.release();
                });
                const auto start = Clock::now();
                auto io = scheduler.acquire({ExecutionKind::background,
                                             ResourceClass::io_heavy,
                                             2,
                                             "I/O work with spare total capacity",
                                             {}});
                const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                if (io.slots.size() != 2 || scheduler.snapshot()["occupied"].get<std::size_t>() > 10)
                    throw Error("Incorrect weighted admission");
                io.release();
                release.get();
                if (scheduler.snapshot()["occupied"] != 0)
                    throw Error("Leaked reservation");
                ++acquired;
                if (i >= 2)
                    samples.push_back(elapsed);
            }
            std::sort(samples.begin(), samples.end());
            std::cout << Json{{"holderClass", resource_name(holder_class)},
                              {"holderDurationMs", 80},
                              {"warmup", 2},
                              {"count", samples.size()},
                              {"acquired", acquired},
                              {"percentileConvention", "median and nearest-rank p95"},
                              {"p50Ms", (samples[9] + samples[10]) / 2.0},
                              {"p95Ms", samples[18]},
                              {"sortedSamplesMs", samples}}
                             .dump()
                      << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
