#include "../src/text_range.hpp"
#include "devbox/storage.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif
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
void batch_read_checks(const fs::path& root) {
    const auto directory = root / "batch-read";
    ensure_directory(directory);
    const auto first = directory / "first.txt", unicode = directory / "unicode.txt",
               empty = directory / "empty.txt", invalid = directory / "invalid.txt";
    const std::string utf8 = "A\xf0\x9f\xa6\x80Z";
    const std::string bad = std::string("bad") + char(0xff);
    write_file(first, "alpha\n");
    write_file(unicode, utf8);
    write_file(empty, "");
    write_file(invalid, bad);
    const std::vector<std::string> paths{path_text(first), path_text(unicode), path_text(empty)};
    auto result = read_text_batch(paths);
    require(result["complete"] == true && result["atomic_snapshot"] == false && result["read_files"] == 3 &&
                result["bytes_read"] == 6 + utf8.size() && result["files"][0]["text"] == "alpha\n" &&
                result["files"][1]["text"] == utf8 && result["files"][2]["text"] == "" &&
                result["files"][1]["read_bytes_sha256"] == sha256(utf8) &&
                result["files"][2]["read_bytes_sha256"] == sha256(""),
            "batch reads preserve input order, Unicode, empty files and exact raw-byte digests");
    result = read_text_batch(paths, 4, 5);
    require(result["complete"] == false && result["bytes_read"] == 5 && result["truncated_files"] == 2 &&
                result["skipped_files"] == 1 && result["files"][0]["text"] == "alph" &&
                result["files"][1]["text"] == "A" && result["files"][2]["status"] == "skipped",
            "aggregate byte budget bounds later files and explicitly skips the remaining paths");
    const std::vector<std::string> partial{path_text(directory / "missing"), path_text(first)};
    result = read_text_batch(partial);
    require(result["errors"] == 1 && result["read_files"] == 1 && result["complete"] == false &&
                result["files"][0]["status"] == "error" && result["files"][1]["text"] == "alpha\n",
            "one missing file retains a later successful read and explicit partial status");
    const std::vector<std::string> nonregular{path_text(directory), path_text(first)};
    require(read_text_batch(nonregular)["errors"] == 1, "directories are not read as regular files");
    const std::vector<std::string> malformed{path_text(invalid)};
    result = read_text_batch(malformed);
    require(result["utf8_replaced_files"] == 1 && result["complete"] == false &&
                result["files"][0]["invalid_utf8_replaced"] == true &&
                result["files"][0]["text"] == sanitize_utf8(bad) &&
                result["files"][0]["read_bytes_sha256"] == sha256(bad),
            "invalid UTF8 replacement is explicit and does not change the raw-byte digest");
    const std::vector<std::string> cut_unicode{path_text(unicode)};
    result = read_text_batch(cut_unicode, 2);
    require(result["files"][0]["bytes_read"] == 2 && result["files"][0]["truncated"] == true &&
                result["files"][0]["invalid_utf8_replaced"] == true &&
                result["files"][0]["read_bytes_sha256"] == sha256(utf8.substr(0, 2)),
            "a byte boundary inside UTF8 retains exact source offsets and honest replacement metadata");
    const std::vector<std::string> none, duplicate{paths[0], paths[0]}, relative{"relative.txt"},
        nul{paths[0] + std::string(1, '\0') + "tail"}, too_many(17, paths[0]);
    rejects([&] { read_text_batch(none); }, "READ_BATCH_LIMIT");
    rejects([&] { read_text_batch(too_many); }, "READ_BATCH_LIMIT");
    rejects([&] { read_text_batch(duplicate); }, "READ_BATCH_PATH");
    rejects([&] { read_text_batch(relative); }, "READ_BATCH_PATH");
    rejects([&] { read_text_batch(nul); }, "READ_BATCH_PATH");
    rejects([&] { read_text_batch(paths, 0); }, "READ_BATCH_LIMIT");
    rejects([&] { read_text_batch(paths, 65537); }, "READ_BATCH_LIMIT");
    rejects([&] { read_text_batch(paths, 8192, 0); }, "READ_BATCH_LIMIT");
    rejects([&] { read_text_batch(paths, 8192, 262145); }, "READ_BATCH_LIMIT");
    auto cancel = std::make_shared<Cancellation>();
    cancel->cancel();
    bool cancelled = false;
    try {
        (void)read_text_batch(paths, 8192, 65536, cancel);
    } catch (const Cancelled&) {
        cancelled = true;
    }
    require(cancelled, "batch read honours pre-existing cancellation");
    require(read_file(first) == "alpha\n" && read_file(unicode) == utf8 && read_file(invalid) == bad &&
                read_file(empty).empty(),
            "all batch operations preserve source bytes");
}
void text_range_checks(const fs::path& root) {
    constexpr auto top = detail::text_range_max_offset;
    const auto at_end = detail::text_range_boundary("", top, 4);
    require(at_end.eof && at_end.bytes == 0, "EOF at the safe integer limit is representable");
    const auto exact_end = detail::text_range_boundary("abc", top - 3, 4);
    require(exact_end.eof && exact_end.bytes == 3, "a final ASCII range may end at the safe limit");
    const auto capped = detail::text_range_boundary("abcd", top - 3, 4);
    require(!capped.eof && capped.bytes == 3 && top - 3 + capped.bytes == top,
            "a continuation offset never exceeds the safe integer limit");
    rejects([&] { detail::text_range_boundary("d", top, 4); }, "READ_TEXT_RANGE_LIMIT");
    rejects([&] { detail::text_range_boundary("\xe7\x95\x8c", top - 2, 4); }, "READ_TEXT_RANGE_LIMIT");
    rejects([&] { detail::text_range_boundary("", top + 1, 4); }, "READ_TEXT_RANGE_LIMIT");
    // Differential EOF coverage against the existing UTF-8 sanitizer, without
    // creating 65,536 files or encoding the decoder's branches in the test.
    for (unsigned first = 0; first < 256; ++first)
        for (unsigned second = 0; second < 256; ++second) {
            const std::string bytes{static_cast<char>(first), static_cast<char>(second)};
            if (sanitize_utf8(bytes) == bytes) {
                const auto result = detail::text_range_boundary(bytes, 0, 4);
                require(result.eof && result.bytes == bytes.size(), "valid two-byte EOF is unchanged");
            } else {
                rejects([&] { detail::text_range_boundary(bytes, 0, 4); }, "READ_TEXT_RANGE_UTF8");
            }
        }
    const auto path = root / "utf8-range.txt";
    const std::string part = "\xef\xbb\xbf"
                             "A\xc2\xa3"
                             "\xf0\x9f\xa6\x80"
                             "\xe7\x95\x8c"
                             "\r\n" +
                             std::string("\0tail", 5);
    const auto content = part + part + part;
    write_file(path, content);
    for (const std::size_t limit : {4, 5, 6, 7, 8, 9, 11, 16, 64, 65536}) {
        std::string assembled;
        std::uint64_t offset = 0;
        for (;;) {
            const auto range = read_text_range(path, offset, limit);
            const auto text = json_string(range, "text");
            const auto next = json_uint(range, "next_offset_bytes");
            require(text.size() <= limit && !text.empty() && next == offset + text.size() &&
                        range["read_bytes_sha256"] == sha256(text) &&
                        range["bytes_returned"] == text.size() &&
                        range["sampled_file_bytes"] == content.size() && range["atomic_snapshot"] == false &&
                        range["encoding"] == "utf-8" && sanitize_utf8(text) == text,
                    "text pages are bounded exact UTF8 with progress and byte digests");
            assembled += text;
            offset = next;
            if (json_bool(range, "eof"))
                break;
            require(json_bool(range, "truncated"), "a remaining range is explicit");
        }
        require(assembled == content, "paging never loses BOM, multibyte scalars, CRLF or NUL");
    }
    const auto first = read_text_range(path, 0, 5);
    require(first["text"] == content.substr(0, 4) && first["next_offset_bytes"] == 4 &&
                first["utf8_boundary_adjusted"] == true,
            "a cut scalar is deferred to the next exact byte offset");
    rejects([&] { read_text_range(path, 5, 4); }, "READ_TEXT_RANGE_OFFSET");
    const auto past = read_text_range(path, 10000, 4);
    require(past["text"] == "" && past["eof"] == true && past["offset_bytes"] == 10000 &&
                past["next_offset_bytes"] == 10000 && past["offset_bytes_requested"] == 10000 &&
                past["read_bytes_sha256"] == sha256(""),
            "past-EOF reads preserve their requested offset and empty digest");
    rejects([&] { read_text_range(path, 0, 3); }, "READ_TEXT_RANGE_LIMIT");
    rejects([&] { read_text_range(path, 0, 65537); }, "READ_TEXT_RANGE_LIMIT");
    rejects([&] { read_text_range(path, std::numeric_limits<std::uint64_t>::max(), 4); },
            "READ_TEXT_RANGE_LIMIT");
    rejects([&] { read_text_range("relative.txt"); }, "READ_TEXT_RANGE_PATH");
    rejects([&] { read_text_range(root / "missing-range"); }, "ENOENT");
    const auto invalid = root / "invalid-range.bin";
    for (const auto& bytes : {std::string("\xff"), std::string("x\xc0\xaf"), std::string("\xed\xa0\x80"),
                              std::string("\xf4\x90\x80\x80"), std::string("x\xf0\x9f")}) {
        write_file(invalid, bytes);
        rejects([&] { read_text_range(invalid); }, "READ_TEXT_RANGE_UTF8");
    }
    for (const auto& partial : {std::string("\xf0\x80"), std::string("\xe0\x80"), std::string("\xed\xa0"),
                                std::string("\xf4\x90")}) {
        write_file(invalid, "ab" + partial + "\x80\x80");
        rejects([&] { read_text_range(invalid, 0, 4); }, "READ_TEXT_RANGE_UTF8");
        write_file(invalid, "abc" + partial + "\x80\x80");
        rejects([&] { read_text_range(invalid, 0, 4); }, "READ_TEXT_RANGE_UTF8");
    }
    for (const auto& scalar : {std::string("\xe0\xa0\x80"), std::string("\xed\x9f\xbf"),
                               std::string("\xf0\x90\x80\x80"), std::string("\xf4\x8f\xbf\xbf")}) {
        write_file(invalid, "ab" + scalar + "tail");
        const auto cut = read_text_range(invalid, 0, 4);
        const auto rest = read_text_range(invalid, json_uint(cut, "next_offset_bytes"));
        require(cut["text"] == "ab" && cut["utf8_boundary_adjusted"] == true && cut["eof"] == false &&
                    rest["text"] == scalar + "tail" && rest["eof"] == true,
                "valid UTF8 edge scalars are deferred and recovered exactly");
    }
    write_file(invalid, "abcd");
    require(read_text_range(invalid, 0, 4)["eof"] == true,
            "lookahead distinguishes exact-budget EOF from remaining bytes");
    write_file(invalid, "abcde");
    require(read_text_range(invalid, 0, 4)["eof"] == false,
            "lookahead detects the first byte beyond the response budget");
#ifdef __linux__
    for (const auto& virtual_path :
         {fs::path("/proc/sys/kernel/ostype"), fs::path("/sys/kernel/uevent_seqnum")}) {
        require(fs::exists(virtual_path), "Linux virtual-file regression fixture is available");
        std::ifstream input(virtual_path, std::ios::binary);
        const std::string expected((std::istreambuf_iterator<char>(input)), {});
        require(!expected.empty(), "virtual fixture has readable bytes");
        const auto range = read_text_range(virtual_path, 0, 65536);
        // uevent_seqnum can change; do not compare values from separate reads.
        require(!json_string(range, "text").empty() && range["eof"] == true &&
                    range["bytes_returned"] == json_string(range, "text").size() &&
                    range["sampled_file_bytes"] != range["bytes_returned"],
                "virtual-file EOF follows actual reads rather than reported size");
        if (virtual_path == fs::path("/proc/sys/kernel/ostype")) {
            const auto first_page = read_text_range(virtual_path, 0, 4);
            const auto last_page =
                read_text_range(virtual_path, json_uint(first_page, "next_offset_bytes"), 4);
            require(json_string(first_page, "text") + json_string(last_page, "text") == expected &&
                        first_page["eof"] == false && last_page["eof"] == true,
                    "zero-size procfs files support exact byte continuation");
        }
    }
#endif
    write_file(invalid, "");
    require(read_text_range(invalid)["eof"] == true, "empty UTF8 files are complete");
    write_file(invalid, std::string(65540, 'q'));
    require(read_text_range(invalid)["bytes_returned"] == 16384 &&
                read_text_range(invalid, 0, 65536)["bytes_returned"] == 65536,
            "default and maximum range budgets remain bounded");
    require(read_file(path) == content, "text range reads never modify the file");
}
#ifndef _WIN32
void small_stack_file_io(const fs::path& root) {
    struct State {
        fs::path file;
        std::exception_ptr error;
    } state{root / "small-stack.bin", {}};
    pthread_attr_t attributes;
    require(pthread_attr_init(&attributes) == 0, "initialize small-stack attributes");
    ScopeExit destroy([&] { pthread_attr_destroy(&attributes); });
    require(pthread_attr_setstacksize(&attributes, 128 * 1024) == 0, "128 KiB worker stack");
    pthread_t worker;
    require(pthread_create(
                &worker, &attributes,
                [](void* opaque) -> void* {
                    auto& state = *static_cast<State*>(opaque);
                    try {
                        std::string payload(3 * 65536, 's');
                        atomic_write(state.file, payload, false, false);
                        atomic_write(state.file, "tail", true, false);
                        payload += "tail";
                        require(file_state(state.file).sha256 == sha256(payload),
                                "small-stack atomic append hash");
                        require(sha256_file(state.file) == sha256(payload), "small-stack streamed hash");
                        require(read_file(state.file) == payload, "small-stack streamed read");
                    } catch (...) {
                        state.error = std::current_exception();
                    }
                    return nullptr;
                },
                &state) == 0,
            "start small-stack storage worker");
    require(pthread_join(worker, nullptr) == 0, "join small-stack storage worker");
    if (state.error)
        std::rethrow_exception(state.error);
}
#endif
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
        batch_read_checks(root);
        text_range_checks(root);
#ifndef _WIN32
        small_stack_file_io(root);
#endif
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
            NativeHandle reader(CreateFileW(target.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
            require(static_cast<bool>(reader), "snapshot reader");
            atomic_write(target, "replaced while reader remains open");
            char previous[9]{};
            DWORD count = 0;
            require(ReadFile(reader.get(), previous, 9, &count, nullptr) &&
                        std::string(previous, count) == "preserved",
                    "atomic replacement preserves existing reader snapshot");
        }
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
