#include "devbox/artifacts.hpp"
#include <cstdlib>
#include <future>
#include <iostream>
using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class F> void rejects(F&& fn, std::string_view expected) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos)
            return;
        throw Error("Unexpected rejection: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(expected));
}
void prepare(const fs::path& root) {
    ensure_directory(root / "workspace");
}
std::string payload(std::size_t index) {
    return std::string(262144, static_cast<char>('a' + index % 26));
}
void ordinary(const fs::path& root) {
    prepare(root);
    auto store = open_state_store(root / "state");
    ArtifactUploads uploads(store, root / "uploads", root / "workspace");
    const auto target = root / "workspace" / "result.bin";
    write_file(target, "old");
    const auto bytes = payload(0) + payload(1);
    auto begun = uploads.begin("operator", "first", "result.bin", bytes.size(), sha256(bytes), sha256("old"));
    require(begun["next_offset_bytes"] == 0 && read_file(target) == "old",
            "begin leaves destination unchanged");
    require(uploads.begin("operator", "first", "result.bin", bytes.size(), sha256(bytes),
                          sha256("old"))["replayed"] == true,
            "begin is replay safe");
    rejects(
        [&] {
            uploads.begin("operator", "first", "result.bin", bytes.size(), sha256("changed"), sha256("old"));
        },
        "ID_CONFLICT");
    rejects([&] { uploads.status("other", "first"); }, "NOT_FOUND");
    rejects([&] { uploads.begin("operator", "escape", root / "outside.bin", 0, sha256(""), "missing"); },
            "OUTSIDE_WORKSPACE");
    rejects([&] { uploads.finalize("operator", "first"); }, "INCOMPLETE");
    rejects([&] { uploads.chunk("operator", "first", 1, payload(0), sha256(payload(0))); }, "OFFSET");
    rejects([&] { uploads.chunk("operator", "first", 0, payload(0), sha256("wrong")); }, "HASH");
    const auto first = uploads.chunk("operator", "first", 0, payload(0), sha256(payload(0)));
    require(first["next_offset_bytes"] == payload(0).size(), "chunk advances only committed offset");
    require(uploads.chunk("operator", "first", 0, payload(0), sha256(payload(0)))["replayed"] == true,
            "chunk receipt is replay safe");
    rejects([&] { uploads.chunk("operator", "first", 0, payload(1), sha256(payload(1))); }, "CHUNK_CONFLICT");
    auto concurrent_a = std::async(std::launch::async, [&] {
        return uploads.chunk("operator", "first", payload(0).size(), payload(1), sha256(payload(1)));
    });
    auto concurrent_b = std::async(std::launch::async, [&] {
        return uploads.chunk("operator", "first", payload(0).size(), payload(1), sha256(payload(1)));
    });
    require(concurrent_a.get()["replayed"] != concurrent_b.get()["replayed"],
            "concurrent duplicate commits once");
    require(read_file(target) == "old", "chunks never modify the published file");
    const auto final = uploads.finalize("operator", "first");
    require(final["status"] == "completed" && final["reservation_held"] == false &&
                read_file(target) == bytes,
            "one publication matches the whole hash and releases reservation");
    write_file(target, "later edit");
    require(uploads.finalize("operator", "first")["replayed"] == true && read_file(target) == "later edit",
            "published receipt never resurrects an old file over a later edit");
    uploads.begin("operator", "conflict", "result.bin", 3, sha256("new"), sha256("old"));
    uploads.chunk("operator", "conflict", 0, "new", sha256("new"));
    rejects([&] { uploads.finalize("operator", "conflict"); }, "VERSION_CONFLICT");
    require(read_file(target) == "later edit", "final CAS preserves an unexpected destination version");
    uploads.begin("operator", "bad_whole", "bad.bin", 3, sha256("bad"), "missing");
    uploads.chunk("operator", "bad_whole", 0, "new", sha256("new"));
    rejects([&] { uploads.finalize("operator", "bad_whole"); }, "WHOLE_HASH");
    require(!fs::exists(root / "workspace" / "bad.bin"), "whole hash failure never publishes");
    uploads.begin("operator", "cancelled", "cancelled.bin", 3, sha256("new"), "missing");
    uploads.chunk("operator", "cancelled", 0, "new", sha256("new"));
    require(uploads.cancel("operator", "cancelled")["reservation_held"] == false,
            "cancel frees staged reservation");
    rejects([&] { uploads.finalize("operator", "cancelled"); }, "NOT_RECEIVING");
    uploads.begin("operator", "empty", "empty.bin", 0, sha256(""), "missing");
    require(uploads.finalize("operator", "empty")["status"] == "completed" &&
                fs::file_size(root / "workspace" / "empty.bin") == 0,
            "zero-byte artifacts finalize without a fake chunk");
    uploads.begin("operator", "tampered", "tampered.bin", 3, sha256("new"), "missing");
    uploads.chunk("operator", "tampered", 0, "new", sha256("new"));
    const auto tampered_key = sha256(Json::array({"operator", "tampered"}).dump());
    write_file(root / "uploads" / tampered_key / "chunk-0", "bad");
    rejects([&] { uploads.finalize("operator", "tampered"); }, "CHUNK_HASH_CHANGED");
    require(!fs::exists(root / "workspace" / "tampered.bin"), "tampered staged content never publishes");
    uploads.cancel("operator", "tampered");
    uploads.begin("operator", "aliased", "aliased.bin", 3, sha256("new"), "missing");
    uploads.chunk("operator", "aliased", 0, "new", sha256("new"));
    const auto alias_key = sha256(Json::array({"operator", "aliased"}).dump());
    const auto alias = root / "uploads" / alias_key / "chunk-0";
    fs::remove(alias);
    write_file(root / "workspace" / "unrelated.bin", "new");
    fs::create_hard_link(root / "workspace" / "unrelated.bin", alias);
    rejects([&] { uploads.finalize("operator", "aliased"); }, "unaliased");
    uploads.cancel("operator", "aliased");
    require(read_file(root / "workspace" / "unrelated.bin") == "new",
            "hardlink refusal and cleanup preserve the other file");
    ArtifactLimits limited;
    limited.active_uploads = 3; // Two deliberately unresolved finalizations above still retain reservations.
    limited.reserved_bytes = 10;
    ArtifactUploads bounded(store, root / "uploads", root / "workspace", limited);
    bounded.begin("operator", "quota", "quota.bin", 4, sha256("four"), "missing");
    rejects([&] { bounded.begin("operator", "quota_two", "quota-two.bin", 1, sha256("x"), "missing"); },
            "AGGREGATE_QUOTA");
    bounded.cancel("operator", "quota");
}
void linear_and_bounded(const fs::path& root) {
    prepare(root);
    auto store = open_state_store(root / "state");
    ArtifactUploads uploads(store, root / "uploads", root / "workspace");
    constexpr std::size_t count = 32;
    std::string all;
    for (std::size_t i = 0; i < count; ++i)
        all += payload(i);
    uploads.begin("operator", "linear", "linear.bin", all.size(), sha256(all), "missing");
    std::vector<fs::path> prior;
    std::vector<fs::file_time_type> timestamps;
    const auto key = sha256(Json::array({"operator", "linear"}).dump());
    for (std::size_t i = 0; i < count; ++i) {
        const auto bytes = payload(i);
        uploads.chunk("operator", "linear", i * bytes.size(), bytes, sha256(bytes));
        for (std::size_t j = 0; j < prior.size(); ++j)
            require(fs::last_write_time(prior[j]) == timestamps[j],
                    "upload never recopies or rewrites a prior chunk");
        prior.push_back(root / "uploads" / key / ("chunk-" + std::to_string(i)));
        timestamps.push_back(fs::last_write_time(prior.back()));
    }
    std::uint64_t staged = 0;
    for (const auto& file : prior)
        staged += fs::file_size(file);
    require(staged == all.size() && !fs::exists(root / "workspace" / "linear.bin"),
            "staged payload is exactly N bytes, no cumulative prefixes");
    require(uploads.finalize("operator", "linear")["status"] == "completed" &&
                sha256_file(root / "workspace" / "linear.bin") == sha256(all),
            "multi-chunk whole-file publication matches independent digest");
    const auto fanout = root / "workspace" / "fanout";
    ensure_directory(fanout);
    for (int i = 0; i < 2500; ++i)
        write_file(fanout / ("entry-" + std::to_string(i)), "");
    ListOptions options;
    options.path = fanout;
    options.max_entries = 10000;
    options.max_enumerated_entries = 100;
    options.memory_budget_bytes = 8192;
    const auto listing = list_files(options);
    require(listing.stdout_text.size() <= 4096 &&
                listing.stderr_text.find("partial listing") != std::string::npos,
            "extreme fanout has independent bounded traversal and explicit partial ordering");
    std::cout << "8 MiB/32 chunks: exactly 8 MiB staged, no prefix rewrite; bounded 2500-entry directory\n";
}
void crash_recovery(const fs::path& root, std::string_view point, bool cancel_after_crash = false) {
    prepare(root);
    const auto target = root / "workspace" / "crash.bin";
    {
        auto store = open_state_store(root / "state");
        ArtifactUploads uploads(store, root / "uploads", root / "workspace");
        write_file(target, "old");
        uploads.begin("operator", "crash", "crash.bin", 9, sha256("crashbody"), sha256("old"));
        if (point != "chunk_durable")
            uploads.chunk("operator", "crash", 0, "crashbody", sha256("crashbody"));
    }
    ProcessOptions options;
    options.timeout = Millis(15000);
    bool crashed = false;
    try {
        spawn_process(path_text(executable_path()), {"--crash", path_text(root), std::string(point)},
                      options);
    } catch (const ProcessError& error) {
        crashed = error.exit_code == 73;
    }
    require(crashed, "child died at exact durable boundary");
    const auto before = fs::last_write_time(target);
    require(read_file(target) == (point == "after_publish" ? "crashbody" : "old"),
            "crash leaves either complete old or new artifact");
    auto store = open_state_store(root / "state");
    ArtifactUploads recovered(store, root / "uploads", root / "workspace");
    if (cancel_after_crash) {
        require(recovered.cancel("operator", "crash")["status"] == "cancelled",
                "cancel can retire an unacknowledged durable chunk");
        const auto key = sha256(Json::array({"operator", "crash"}).dump());
        require(!fs::exists(root / "uploads" / key / "chunk-0") && read_file(target) == "old",
                "orphan chunk removed without publication");
        require(store->get("artifact_quota", "global")->data["bytes"] == 0,
                "orphan cleanup releases reservation");
        return;
    }
    if (point == "chunk_durable")
        recovered.chunk("operator", "crash", 0, "crashbody", sha256("crashbody"));
    require(recovered.finalize("operator", "crash")["status"] == "completed" &&
                read_file(target) == "crashbody",
            "restart recovers staged chunks and final publication");
    if (point == "after_publish")
        require(fs::last_write_time(target) == before,
                "lost publication acknowledgement reconciles without republishing");
    require(store->get("artifact_quota", "global")->data["bytes"] == 0,
            "recovered completion releases quota once");
}
} // namespace
int run(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--crash") {
        const auto root = path_from_utf8(argv[2]);
        const std::string point = argv[3];
        ArtifactLimits limits;
        limits.transition_hook = [point](std::string_view stage) {
            if (point == stage)
                std::_Exit(73);
        };
        auto store = open_state_store(root / "state");
        ArtifactUploads uploads(store, root / "uploads", root / "workspace", limits);
        if (point == "chunk_durable")
            uploads.chunk("operator", "crash", 0, "crashbody", sha256("crashbody"));
        else
            uploads.finalize("operator", "crash");
        return 2;
    }
    const auto root = fs::temp_directory_path() / ("devbox-artifacts-" + uuid());
    try {
        ordinary(root / "ordinary");
        linear_and_bounded(root / "linear");
        for (const auto* point : {"chunk_durable", "before_publish", "after_publish"})
            crash_recovery(root / point, point);
        crash_recovery(root / "cancel_after_chunk", "chunk_durable", true);
        fs::remove_all(root);
        std::cout << "Resumable uploads, CAS publication, quota, replay, crash recovery and bounded "
                     "directory tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(narrow(argv[i]));
    std::vector<char*> values;
    for (auto& arg : args)
        values.push_back(arg.data());
    return run(argc, values.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
