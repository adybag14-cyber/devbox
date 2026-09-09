#include "devbox/native.hpp"
#include "devbox/result.hpp"
#include "devbox/telemetry.hpp"
#include <future>
#include <iostream>
#ifdef _WIN32
#include <tlhelp32.h>
#endif
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class Predicate> void until(Predicate check) {
    const auto deadline = Clock::now() + Millis(3000);
    while (!check()) {
        if (Clock::now() > deadline)
            throw Error("Telemetry condition timed out");
        std::this_thread::sleep_for(Millis(5));
    }
}
} // namespace
int main() {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-telemetry-" + uuid());
    fs::create_directories(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        const auto args = summarize_arguments(Json{{"command", std::string(239, 'a') + "😀z"},
                                                   {"nested", {{"refresh_token", "NEVER-LOG-THIS"}}},
                                                   {"content_base64", "SECRET-BYTES"},
                                                   {"signal", "internal"},
                                                   {"values", Json::array({1, 2, 3, 4, 5, 6, 7, 8, 9})}});
        require(!args.contains("signal") && args["command"]["length"] == 242 &&
                    args["command"]["preview"] == std::string(239, 'a') + "...",
                "UTF16 bounded argument preview");
        require(args.dump().find("NEVER-LOG-THIS") == std::string::npos &&
                    args["content_base64"]["redacted"] == true && args["values"]["sample"].size() == 8,
                "nested argument redaction and bounded arrays");
        JsonLogSink sink(root / "rotation.jsonl", 30, 2);
        for (int i = 0; i < 12; ++i)
            sink.append(Json{{"value", i}});
        require(fs::exists(root / "rotation.jsonl.2") && !fs::exists(root / "rotation.jsonl.3") &&
                    fs::file_size(root / "rotation.jsonl") < 30,
                "usage rotation bounded");
        JsonLogSink batched(root / "batch.jsonl", 30, 2);
        std::vector<Json> records;
        for (int i = 0; i < 12; ++i)
            records.push_back(Json{{"value", i}});
        batched.append_batch(records);
        for (const auto* suffix : {"", ".1", ".2"})
            require(read_file(root / path_from_utf8(std::string("rotation.jsonl") + suffix)) ==
                        read_file(root / path_from_utf8(std::string("batch.jsonl") + suffix)),
                    "batched logs preserve exact event ordering, flush and rotation boundaries");
        {
            JsonLogSink mixed(root / "mixed-batch.jsonl", 0, 0);
            std::vector<Json> events;
            std::string expected;
            for (int i = 0; i < 256; ++i) {
                events.push_back(Json{{"sequence", i}, {"payload", std::string(600, 'x')},
                                      {"escaped", "\"\\\n\t"}, {"unicode", "é😀"},
                                      {"invalid", std::string("a\xffz", 3)}, {"fraction", i / 3.0}});
                expected += events.back().dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
            }
            mixed.append_batch(events);
            require(read_file(root / "mixed-batch.jsonl") == expected,
                    "large log batches preserve reference JSON bytes across multiple flushes");
        }
        BackgroundTasks background;
        {
            UsageLogger burst(root / "burst.jsonl", 1024 * 1024, 1, background, "burst-writer");
            for (int i = 0; i < 512; ++i)
                burst.enqueue(Json{{"sequence", i}});
            burst.stop();
            const auto lines = split(read_file(root / "burst.jsonl"), '\n');
            for (std::size_t i = 0; i < 512; ++i)
                require(Json::parse(lines.at(i))["sequence"] == i, "queued log event order and completeness");
            require(burst.snapshot()["enqueued"] == 512 && burst.snapshot()["dropped"] == 0 &&
                        burst.snapshot()["writeFailures"] == 0,
                    "bounded burst drains without telemetry loss");
        }
        // A failed file open must increment failure state; a later event must recover.
        const auto path = root / "blocked.jsonl";
        fs::create_directory(path);
        UsageLogger writer(path, 1000, 1, background, "probe-writer");
        writer.enqueue(Json{{"one", 1}});
        until([&] { return json_uint(writer.snapshot(), "writeFailures") == 1; });
        require(background.snapshot()["probe-writer"]["consecutiveFailures"] == 1, "writer failure visible");
        fs::remove(path);
        writer.enqueue(Json{{"two", 2}});
        until([&] { return background.snapshot()["probe-writer"]["consecutiveFailures"] == 0; });
        writer.stop();
        require(read_file(path).find("two") != std::string::npos && writer.snapshot()["writeFailures"] == 1,
                "writer recovers without masking historical failure");
        Config config;
        config.project_root = root;
        config.mcp_performance_state_path = root / "perf.json";
        UsageTelemetry usage(config, background);
        const auto id = usage.started("host_exec", Json{{"password", "PASSWORD"}}, Json{{"request_id", 7}});
        require(usage.active_tools().size() == 1, "active invocation registered");
        usage.finished(id, result_process("done", Json{{"execution", {{"queue_wait_ms", 12}, {"slot", 3}}}},
                                          "stdout", "stderr", 2, false));
        const auto failure = usage.started("devbox_wait", Json::object(), Json::object());
        usage.failed(failure, "cancelled");
        HttpRequest request;
        request.method = "POST";
        request.path = "/mcp";
        request.query = "secret=INQUERY";
        request.headers = {{"authorization", "BEARERSECRET"}, {"x-forwarded-for", "1.2.3.4, 5.6.7.8"}};
        usage.http(request, 200, Millis(123), true);
        usage.stop();
        require(usage.active_tools().empty(), "terminal invocation removal");
        const auto log = read_file(root / "run" / "tool-usage.jsonl");
        require(log.find("PASSWORD") == std::string::npos && log.find("tool_throw") != std::string::npos &&
                    log.find("\"queue_wait_ms\":12") != std::string::npos,
                "tool lifecycle metadata logged and redacted");
        const auto http = read_json(root / "run" / "http-usage.jsonl");
        require(http["status_code"].is_null() && http["client_aborted"] == true &&
                    http["forwarded_for"] == "1.2.3.4" && http.dump().find("SECRET") == std::string::npos &&
                    http.dump().find("INQUERY") == std::string::npos,
                "HTTP disconnect metadata excludes secrets");
        const auto before = allocator_snapshot();
        void* allocation = ::operator new(65536, std::align_val_t(256));
        const auto during = allocator_snapshot();
        require(reinterpret_cast<std::uintptr_t>(allocation) % 256 == 0, "aligned new alignment");
        ::operator delete(allocation, std::align_val_t(256));
        const auto after = allocator_snapshot();
        if (json_string(before, "backend") == "cpp-global-new")
            require(json_uint(during, "cumulativeAllocatedBytes") >=
                            json_uint(before, "cumulativeAllocatedBytes") + 65536 &&
                        json_uint(after, "cumulativeFreedBytes") >=
                            json_uint(during, "cumulativeFreedBytes") + 65536,
                    "global allocator observes actual allocations and frees");
        asio::io_context io(1);
        auto work = asio::make_work_guard(io);
        auto cancel = std::make_shared<Cancellation>();
#ifdef _WIN32
        std::promise<void> release_threads;
        auto gate = release_threads.get_future().share();
        std::vector<std::future<void>> live_threads;
        ScopeExit release([&] { release_threads.set_value(); });
        std::atomic_int entered{0};
        for (int i = 0; i < 3; ++i)
            live_threads.push_back(std::async(std::launch::async, [&, gate] {
                ++entered;
                gate.wait();
            }));
        until([&] { return entered == 3; });
        std::uint32_t expected_threads = 0;
        {
            NativeHandle reference(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            require(reference && Thread32First(reference.get(), &entry), "reference thread snapshot");
            do {
                expected_threads += entry.th32OwnerProcessID == process_id();
            } while (Thread32Next(reference.get(), &entry));
        }
#endif
        PerformanceMonitor monitor(config, background, [] { return Json{{"gitSha", "test"}}; });
#ifdef _WIN32
        require(expected_threads >= 4 &&
                    monitor.snapshot()["process"]["platform"]["threads"] == expected_threads,
                "process thread metric matches independent Toolhelp enumeration with live workers");
        release_threads.set_value();
        release.disarm();
        for (auto& thread : live_threads)
            thread.get();
#endif
        monitor.attach(io.get_executor(), cancel);
        std::thread loop([&] { io.run(); });
        ScopeExit stop([&] {
            cancel->cancel();
            work.reset();
            if (loop.joinable())
                loop.join();
            background.stop();
        });
        std::this_thread::sleep_for(Millis(80));
        std::promise<void> stalled;
        auto done = stalled.get_future();
        asio::post(io, [&] {
            std::this_thread::sleep_for(Millis(180));
            stalled.set_value();
        });
        done.get();
        std::this_thread::sleep_for(Millis(60));
        monitor.persist();
        const auto metrics = monitor.snapshot();
        require(json_number(metrics["eventLoop"], "maxMs") > 120 &&
                    json_uint(metrics["eventLoop"], "sampleCount") > 3,
                "sampler measures actual serving executor stall");
        require(json_uint(metrics["process"]["memory"], "rss") > 0 && !metrics["stale"].get<bool>() &&
                    read_json(config.mcp_performance_state_path).contains("process"),
                "process metrics and durable performance snapshot");
        cancel->cancel();
        work.reset();
        loop.join();
        background.stop();
        stop.disarm();
        std::cout << "Telemetry redaction, rotation, writer recovery, allocator and real event-loop sampling "
                     "passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
