// Native worker versus in-process diagnostic. Neither lane touches production.
// Operation timings are captured inside the child after its input is received;
// outer timings include serialization, spawn, IPC, capture and validation.
#include "devbox/contract.hpp"
#include "devbox/filesystem_worker.hpp"
#include "devbox/native.hpp"
#include "devbox/storage.hpp"
#include "devbox/telemetry.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
using namespace devbox;
namespace {
Json dispatch(std::string_view operation, const Json& args) {
    const auto start = Clock::now();
    const auto value = operation == "noop" ? Json{{"marker", 42}} : filesystem_operation(operation, args);
    return Json{{"value", value},
                {"operation_us", std::chrono::duration<double, std::micro>(Clock::now() - start).count()}};
}
template <class Work> void measure(const std::string& name, unsigned count, Work work) {
    std::vector<double> elapsed, child;
    const auto allocations_before = allocator_snapshot();
    for (unsigned i = 0; i < count + 5; ++i) {
        const auto start = Clock::now();
        const auto inner = work(i);
        const auto us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        if (i >= 5) {
            elapsed.push_back(us);
            child.push_back(inner);
        }
    }
    auto statistics = [](auto values) {
        std::sort(values.begin(), values.end());
        return Json{{"p50_us", values[(values.size() - 1) / 2]},
                    {"p95_us", values[(values.size() - 1) * 95 / 100]},
                    {"mean_us", std::accumulate(values.begin(), values.end(), 0.0) / values.size()},
                    {"samples_us", values}};
    };
    const auto allocations_after = allocator_snapshot();
    std::cout << Json{{"case", name},
                      {"samples", count},
                      {"outer", statistics(elapsed)},
                      {"operation", statistics(child)},
                      {"parent_allocation_calls_including_5_warmups",
                       json_uint(allocations_after, "allocationCalls") -
                           json_uint(allocations_before, "allocationCalls")},
                      {"parent_allocated_bytes_including_5_warmups",
                       json_uint(allocations_after, "cumulativeAllocatedBytes") -
                           json_uint(allocations_before, "cumulativeAllocatedBytes")}}
                     .dump()
              << '\n'
              << std::flush;
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker(dispatch);
    try {
        if (argc != 2)
            throw Error("Supply a new absolute fixture directory");
        const auto root = path_from_utf8(argv[1]);
        if (!root.is_absolute() || !fs::create_directory(root))
            throw Error("New fixture required");
        std::cout << Json{{"build", build_snapshot()},
                          {"fixture", path_text(root)},
                          {"note",
                           "Diagnostic direct lane has no worker isolation; never a production replacement"}}
                         .dump()
                  << '\n';
        const std::string capture_payload(1024 * 1024, 'c');
        measure("zero_capture_1m", 20, [&](auto) {
            CaptureAccumulator capture(0);
            for (std::size_t offset = 0; offset < capture_payload.size(); offset += 16384)
                capture.push(std::string_view(capture_payload).substr(offset, 16384));
            capture.finish();
            const auto result = capture.snapshot();
            if (result.original_chars != capture_payload.size() || !result.text.empty() || !result.truncated)
                throw Error("Zero capture metadata mismatch");
            return 0.0;
        });
        measure("worker_noop", 100, [&](auto) {
            const auto value = isolated_filesystem("noop", Json::object(), Millis(5000));
            if (value["value"]["marker"] != 42)
                throw Error("Worker protocol mismatch");
            return value["operation_us"].template get<double>();
        });
        for (const auto size : {4096U, 32768U, 524288U}) {
            const std::string payload(size, 'x');
            const auto path = root / ("read-" + std::to_string(size) + ".bin");
            write_file(path, payload);
            const auto encoded = base64_encode(payload);
            const Json args{{"path", path_text(path)}, {"max_bytes", size}};
            for (const auto isolated : {false, true})
                measure(std::string(isolated ? "worker_read_" : "direct_read_") + std::to_string(size), 100,
                        [&](auto) {
                            const auto value = isolated
                                                   ? isolated_filesystem("read_large", args, Millis(5000))
                                                   : dispatch("read_large", args);
                            if (value["value"]["content_base64"] != encoded)
                                throw Error("Read byte mismatch");
                            return value["operation_us"].template get<double>();
                        });
        }
        for (const auto size : {4096U, 32768U, 65537U, 1048576U}) {
            const std::string payload(size, 'w');
            const auto encoded = base64_encode(payload), digest = sha256(payload);
            for (const auto isolated : {false, true})
                measure(std::string(isolated ? "worker_write_" : "direct_write_") + std::to_string(size), 20,
                        [&](auto i) {
                            const auto path =
                                root / (std::string(isolated ? "worker-" : "direct-") + std::to_string(size) +
                                        "-" + std::to_string(i) + ".bin");
                            const Json args{{"path", path_text(path)},
                                            {"content_base64", encoded},
                                            {"expected_file_sha256", "missing"}};
                            const auto value = isolated
                                                   ? isolated_filesystem("atomic_write", args, Millis(10000))
                                                   : dispatch("atomic_write", args);
                            if (value["value"]["replayed"] != false ||
                                value["value"]["current"]["sha256"] != digest ||
                                value["value"]["current"]["bytes"] != size)
                                throw Error("Atomic receipt mismatch");
                            return value["operation_us"].template get<double>();
                        });
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
