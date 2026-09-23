#include "devbox/filesystem_worker.hpp"
#include <future>
#include <iostream>
using namespace devbox;
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw Error(message);
}
template <class F> void rejects(F&& work, std::string_view part) {
    try {
        work();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(part) != std::string_view::npos)
            return;
        throw;
    }
    throw Error("expected rejection: " + std::string(part));
}
Json fixture(std::string_view operation, const Json& args) {
    if (operation == "fixture_stuck") {
        const auto pid = process_id();
        write_json_atomic(path_from_utf8(json_string(args, "path")),
                          Json{{"pid", pid}, {"instance", *process_instance(pid)}});
        for (;;)
            std::this_thread::sleep_for(Millis(1000));
    }
    return filesystem_operation(operation, args);
}
void dead(const fs::path& receipt) {
    const auto owner = read_json_optional(receipt);
    require(owner.has_value(), "owned worker admission receipt missing");
    require(!process_matches_instance(json_uint(*owner, "pid"), json_uint(*owner, "instance")),
            "filesystem worker still alive after termination acknowledgement");
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--filesystem-worker")
        return run_filesystem_worker(fixture);
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-fs-worker-" + uuid());
    fs::create_directory(root);
    ScopeExit clean([&] {
        std::error_code error;
        fs::remove_all(root, error);
    });
    try {
        const auto path = path_text(root / "roundtrip.bin");
        const std::string payload("line\r\n\0\xff", 8);
        Json args{
            {"path", path}, {"content_base64", base64_encode(payload)}, {"expected_file_sha256", "missing"}};
        const auto first = isolated_filesystem("atomic_write", args, Millis(5000));
        require(first["current"]["sha256"] == sha256(payload), "exact binary worker write");
        require(isolated_filesystem("atomic_write", args, Millis(5000))["replayed"] == true,
                "write CAS replay survives worker replacement");
        args["content_base64"] = base64_encode("different");
        rejects([&] { isolated_filesystem("atomic_write", args, Millis(5000)); }, "conflict");
        require(isolated_filesystem("read_large", Json{{"path", path}}, Millis(5000))["content_base64"] ==
                    base64_encode(payload),
                "binary roundtrip");
        {
            Json nested = Json::array();
            for (int i = 0; i < 500; ++i)
                nested.push_back("checkpoint");
            for (int depth = 0; depth < 60; ++depth)
                nested = Json{{"next", std::move(nested)}};
            Json request{{"task_id", "nested"}, {"expected_revision", 0}, {"state", nested}};
            const auto task_root = path_text(root / "tasks");
            const auto result = isolated_filesystem(
                "legacy_task", Json{{"root", task_root}, {"action", "put"}, {"request", request}},
                Millis(5000));
            require(result["record"]["state"] == nested, "deep legacy checkpoint survives worker envelopes");
            Json leaves = Json::array();
            for (int i = 0; i < 20000; ++i)
                leaves.push_back(0);
            request = Json{{"task_id", "many-leaves"}, {"expected_revision", 0}, {"state", leaves}};
            const auto wide = isolated_filesystem(
                "legacy_task", Json{{"root", task_root}, {"action", "put"}, {"request", request}},
                Millis(5000));
            require(wide["record"]["state"] == leaves,
                    "legal sub-64KiB checkpoint preserves its node capacity");
        }
        rejects(
            [&] {
                isolated_filesystem("read_large", Json{{"path", path}, {"max_bytes", 9000000}}, Millis(5000));
            },
            "READ_BUDGET");
        const auto large = root / "large.txt";
        write_file(large, std::string(4 * 1024 * 1024, 'x'));
        require(isolated_filesystem("read_text",
                                    Json{{"path", path_text(large)}, {"max_bytes", 4 * 1024 * 1024}},
                                    Millis(5000))["text"]
                        .get_ref<const std::string&>()
                        .size() == 4 * 1024 * 1024,
                "bounded raw pipe captures a multi-megabyte response");
        const auto timed = root / "deadline-owner.json";
        const auto started = Clock::now();
        rejects([&] { isolated_filesystem("fixture_stuck", Json{{"path", path_text(timed)}}, Millis(500)); },
                "FILESYSTEM_WORKER_DEADLINE");
        require(Clock::now() - started < Millis(2500), "stuck-worker deadline plus escalation bounded");
        dead(timed);
        const auto cancelled = root / "cancel-owner.json";
        auto cancel = std::make_shared<Cancellation>();
        auto work = std::async(std::launch::async, [&] {
            try {
                isolated_filesystem("fixture_stuck", Json{{"path", path_text(cancelled)}}, Millis(10000),
                                    cancel);
            } catch (const Cancelled&) {
                return true;
            }
            return false;
        });
        const auto ready_deadline = Clock::now() + Millis(3000);
        while (!fs::exists(cancelled) && Clock::now() < ready_deadline)
            std::this_thread::sleep_for(Millis(5));
        const auto stopped = Clock::now();
        cancel->cancel();
        require(work.get() && Clock::now() - stopped < Millis(2000),
                "stuck worker cancellation acknowledged promptly");
        dead(cancelled);
        std::cout << "Filesystem process bounds, exact bytes, CAS replay, deadline and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
