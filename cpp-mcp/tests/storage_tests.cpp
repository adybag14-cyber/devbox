#include "devbox/storage.hpp"
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class F> void rejects(F&& operation, std::string_view part) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(part) != std::string_view::npos)
            return;
        throw Error("Unexpected error: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(part));
}
int run(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--append") {
        for (int i = 0; i < 12; ++i)
            atomic_write(path_from_utf8(argv[2]), argv[3], true, false);
        return 0;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-cpp-storage-" + uuid());
    fs::create_directory(root);
#ifndef _WIN32
    fs::permissions(root, fs::perms::owner_all);
#endif
#ifdef _WIN32
    const std::string profile_key = "LOCALAPPDATA";
#else
    const std::string profile_key = "HOME";
#endif
    const auto saved_profile = environment(profile_key);
    set_environment(profile_key, path_text(root));
    ScopeExit cleanup([&] {
        set_environment(profile_key, saved_profile);
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        const auto target = root / "target.bin";
        require(!file_state(target).exists, "missing file state");
        const auto first = atomic_write(target, "hello", false, false, {"missing", {}});
        require(first.current.sha256 == sha256("hello") && first.current.bytes == 5 && !first.previous.exists,
                "create-only receipt");
        require(atomic_write(target, "hello", false, false, {"missing", {}}).replayed,
                "identical overwrite replay");
        rejects([&] { atomic_write(target, "different", false, false, {"missing", {}}); },
                "version conflict");
        const Preconditions version{first.current.sha256, 5};
        const auto appended = atomic_write(target, " world", true, false, version);
        require(!appended.replayed && appended.current.bytes == 11, "conditional append");
        require(atomic_write(target, " world", true, false, version).replayed &&
                    read_file(target) == "hello world",
                "conditional append replay");
        rejects([&] { atomic_write(target, "!", true, false, version); }, "version conflict");
        rejects([&] { atomic_write(target, "x", false, false, {{}, 0}); }, "requires append");
        auto page = read_large(target, 6, 5);
        require(page["content_base64"] == base64_encode("world") && page["eof"] == true &&
                    page["next_offset_bytes"] == 11,
                "binary read page");
        require(read_large(target, 100, 10)["bytes_returned"] == 0, "clamped read offset");
        const auto payload = std::string("\0\xff\x01hello", 8);
        auto written = write_large(target, base64_encode(payload), false, false, sha256(payload));
        require(written["verified"] == true && read_file(target) == payload, "binary write verified");
        rejects([&] { write_large(target, "aGVsbG8", false, false); }, "base64");
        rejects([&] { write_large(target, base64_encode("bad"), false, false, sha256("good")); },
                "did not match");
        const auto alias = root / "hardlink.bin";
        fs::create_hard_link(target, alias);
        rejects([&] { atomic_write(target, "overwrite"); }, "hard-linked");
        require(read_file(alias) == payload, "hard link preserved");
        fs::remove(alias);
        const auto link = root / "symlink.bin";
        std::error_code symlink_error;
        fs::create_symlink(target, link, symlink_error);
        if (!symlink_error) {
            require(atomic_lock_stripe(canonical_target(link)) ==
                        atomic_lock_stripe(canonical_target(target)),
                    "alias lock identity");
            atomic_write(link, "through symlink");
            require(fs::is_symlink(link) && read_file(target) == "through symlink",
                    "symlink target replacement");
            fs::remove(link);
            fs::create_symlink(root / "missing.bin", link);
            rejects([&] { atomic_write(link, "no"); }, "dangling symlinks");
            fs::remove(link);
        }
#ifdef _WIN32
        atomic_write(target, "preserved");
        {
            NativeHandle blocker(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             nullptr, OPEN_EXISTING, 0, nullptr));
            require(static_cast<bool>(blocker), "replacement blocker");
            rejects([&] { atomic_write(target, std::string(1024 * 1024, 'x')); }, "os error");
            require(read_file(target) == "preserved", "failed commit preserves original");
            for (const auto& entry : fs::directory_iterator(root))
                require(!path_text(entry.path().filename()).starts_with(".devbox-write-"),
                        "failed stage cleanup");
        }
#endif
        atomic_write(target, "");
        const auto self = path_text(executable_path());
        auto append_process = [&](const std::string& payload_text) {
            ProcessOptions options;
            options.timeout = Millis(15000);
            options.max_capture_chars = 4096;
            return spawn_process(self, {"--append", path_text(target), payload_text}, options);
        };
        auto left = std::async(std::launch::async, [&] { return append_process("A"); });
        auto right = std::async(std::launch::async, [&] { return append_process("B"); });
        left.get();
        right.get();
        const auto concurrent = read_file(target);
        require(concurrent.size() == 24 && std::count(concurrent.begin(), concurrent.end(), 'A') == 12 &&
                    std::count(concurrent.begin(), concurrent.end(), 'B') == 12,
                "cross-process append serialization");
        std::cout
            << "PASS atomic CAS, retries, binary paging, aliases, failed commits and process contention\n"
            << std::flush;
        const auto tasks = root / "tasks";
        require(task_get(tasks, "task-a")["revision"] == 0, "missing task revision");
        const auto state = Json{{"step", "build"}, {"job_id", "job-existing"}};
        auto record = task_put(tasks, "task-a", 0, state);
        require(record["record"]["revision"] == 1, "task first revision");
        require(task_put(tasks, "task-a", 0, state)["replayed"] == true, "task lost-response replay");
        require(task_put(tasks, "task-a", 0,
                         Json{{"job_id", "job-existing"}, {"step", "build"}})["replayed"] == true,
                "task map order independence");
        rejects([&] { task_put(tasks, "task-a", 0, Json{{"step", "other"}}); }, "TASK_CONFLICT");
        rejects([&] { task_put(tasks, "task-a", max_safe_integer, state); }, "integer range");
        rejects([&] { task_put(tasks, "task-b", 0, std::string(65537, 'x')); }, "65536 bytes");
        rejects([&] { task_get(tasks, "../escape"); }, "lowercase ASCII");
        task_put(tasks, "task-b", 0, nullptr);
        const auto listing = task_list(tasks, {}, 1);
        require(listing["tasks"].size() == 1 && listing["next_cursor"] == "task-a", "task pagination cursor");
        require(task_list(tasks, "task-a", 1)["tasks"][0]["task_id"] == "task-b",
                "task pagination continuation");
        require(task_get(tasks, "task-a")["sha256"] == sha256_file(tasks / "task-a.json"),
                "durable task file hash");
        ListOptions list;
        list.path = tasks;
        list.max_entries = 1;
        auto files = list_files(list);
        require(files.stderr_text == "listing capped at 1 entries\n", "file listing cap notice");
        auto cancel = std::make_shared<Cancellation>();
        cancel->cancel();
        rejects([&] { list_files(list, cancel); }, "cancelled");
        std::cout << "PASS task persistence, revisions, pagination, data limits and listing cancellation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i)
        arguments.push_back(narrow(wide_argv[i]));
    std::vector<char*> argv;
    for (auto& value : arguments)
        argv.push_back(value.data());
    return run(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
