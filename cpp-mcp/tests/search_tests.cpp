#include "devbox/native.hpp"
#include "devbox/search.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <set>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
std::set<std::string> lines(const std::string& value) {
    const auto rows = split(value, '\n', false);
    return std::set<std::string>(rows.begin(), rows.end());
}
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-cpp-search-" + uuid());
    try {
        fs::create_directories(root / "nested");
        fs::create_directories(root / "node_modules");
        write_file(root / "a.txt", "first needle\nsecond NEEDLE\nplain\n");
        write_file(root / "nested/b.txt", "needle café\n[UNTERMINATED marker\n");
        write_file(root / "node_modules/excluded.txt", "needle excluded\n");
        write_file(root / "binary.txt", std::string("binary\0needle", 13));
        write_file(root / "too-large.txt", std::string(65537, 'a') + "needle");
        auto config = std::make_shared<Config>();
        config->platform = Platform::detect();
        config->runtime_mode = RuntimeMode::host;
        config->host_exec_enabled = true;
        config->host_search_backend = "native";
        config->devbox_workspace_path = root;
        config->host_default_workdir = root;
        config->power_shell_exe = "powershell.exe";
        config->power_shell_fallback_exe = "powershell.exe";
        config->host_shell = "cmd.exe";
        SearchService search(config);
        RuntimeExecutor runtime(config);
        SearchRequest request;
        request.pattern = "needle";
        request.path = path_text(root);
        request.glob = "*.txt";
        request.max_matches = 20;
        request.max_file_bytes = 65536;
        request.exclude_directories = {"node_modules"};
        const auto native = search.search(request);
        require(split(native.stdout_text, '\n', false).size() == 3, "native search matches and case folding");
        require(native.stderr_text.find("pruned 1") != native.stderr_text.npos &&
                    native.stderr_text.find("oversized") != native.stderr_text.npos,
                "native search exclusions and byte bound");
        request.pattern = "[unterminated";
        require(search.search(request).stdout_text.find("[UNTERMINATED marker") != std::string::npos,
                "invalid regex literal fallback");
        request.pattern = "CAFÉ";
        require(search.search(request).stdout_text.find("café") != std::string::npos,
                "Unicode case-insensitive native pattern");
        request.pattern = "needle";
        request.max_matches = 2;
        const auto limited = search.search(request);
        require(split(limited.stdout_text, '\n', false).size() == 2 &&
                    limited.stderr_text.find("match limit 2") != limited.stderr_text.npos,
                "global match cap");
        request.max_matches = 20;
        request.max_depth = 1;
        require(split(search.search(request).stdout_text, '\n', false).size() == 2, "native depth bound");
        request.max_depth = 12;
        if (find_program("rg")) {
            config->host_search_backend = "auto";
            const auto rg = search.search(request);
            require(lines(rg.stdout_text) == lines(native.stdout_text), "ripgrep and native fixture parity");
            require(rg.stderr_text.find("backend ripgrep") != rg.stderr_text.npos, "ripgrep execution used");
            request.pattern = "[unterminated";
            require(search.search(request).stderr_text.find("invalid regex treated as literal text") !=
                        std::string::npos,
                    "ripgrep literal mode");
            request.pattern = "needle";
            request.max_matches = 1;
            const auto one = search.search(request);
            require(split(one.stdout_text, '\n', false).size() == 1,
                    "ripgrep match-limit termination is successful");
        }
        config->host_search_backend = "native";
        write_file(root / "linear.txt", std::string(200000, 'a') + "X\n");
        request.path = path_text(root / "linear.txt");
        request.pattern = "(a+)+$";
        request.max_file_bytes = 1000000;
        const auto linear_start = Clock::now();
        require(search.search(request).stdout_text.empty(), "nested repetitions do not falsely match");
        require(Clock::now() - linear_start < Millis(1000), "native regex avoids exponential backtracking");
        const auto parent = std::make_shared<Cancellation>(), child = std::make_shared<Cancellation>(parent);
        auto waiting = std::async(std::launch::async, [child] { return child->wait_for(Millis(10000)); });
        parent->cancel();
        require(waiting.wait_for(Millis(100)) == std::future_status::ready && waiting.get(),
                "linked cancellation wakes within bounded interval");
        bool cancelled = false;
        try {
            search.search(request, parent);
        } catch (const Cancelled&) {
            cancelled = true;
        }
        require(cancelled, "search honors cancellation");

        InspectFileRequest inspection;
        inspection.path = "missing.txt";
        inspection.working_dir = root;
        require(inspect_host_file(*config, runtime, inspection)["exists"] == false,
                "missing file inspection");
        inspection.path = "a.txt";
        auto inspected = inspect_host_file(*config, runtime, inspection);
        require(inspected["utf8_valid"] == true && inspected["line_endings"] == "lf" &&
                    inspected["likely_corrupted_on_disk"] == false,
                "valid source inspection");
        require(inspected["sha256"] == sha256_file(root / "a.txt"), "inspection hashes exact bytes");
        write_file(root / "mojibake.txt", "â€”\r\nÃ Â\n");
        inspection.path = "mojibake.txt";
        inspected = inspect_host_file(*config, runtime, inspection);
        require(inspected["suspicious_mojibake_count"] == 1 && inspected["line_endings"] == "mixed" &&
                    inspected["likely_corrupted_on_disk"] == true,
                "mojibake and mixed endings");
        write_file(root / "latin.txt", "Ã Â");
        inspection.path = "latin.txt";
        require(inspect_host_file(*config, runtime, inspection)["suspicious_mojibake_count"] == 0,
                "Latin letters alone are not corruption");
        write_file(root / "invalid.txt", std::string("\xff\0a", 3));
        inspection.path = "invalid.txt";
        inspected = inspect_host_file(*config, runtime, inspection);
        require(inspected["utf8_valid"] == false && inspected["null_byte_count"] == 1 &&
                    inspected["replacement_character_count"] == 1,
                "exact invalid byte signals");
        write_file(root / "program.exe", std::string("MZ\0binary", 9));
        inspection.path = "program.exe";
        inspected = inspect_host_file(*config, runtime, inspection);
        require(inspected["binary_format"] == "pe" && inspected["text_inspection_skipped"] == true &&
                    inspected["utf8_valid"].is_null(),
                "binary magic skips text heuristics");
        write_file(root / "bom.txt", "\xef\xbb\xbf"
                                     "text\r\n");
        inspection.path = "bom.txt";
        require(inspect_host_file(*config, runtime, inspection)["bom"] == "utf8", "UTF8 BOM inspection");
        const auto timestamp_path = root / "timestamp.txt";
        write_file(timestamp_path, "timestamp");
#ifdef _WIN32
        {
            NativeHandle handle(CreateFileW(timestamp_path.c_str(), FILE_WRITE_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
            require(static_cast<bool>(handle), "timestamp fixture open");
            const std::uint64_t ticks = 116444736000000000ULL + 7359700ULL;
            FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
            require(SetFileTime(handle.get(), nullptr, nullptr, &time) != 0, "timestamp fixture set");
        }
#else
        const timespec times[2] = {{0, 735970000}, {0, 735970000}};
        require(utimensat(AT_FDCWD, timestamp_path.c_str(), times, 0) == 0, "timestamp fixture set");
#endif
        inspection.path = "timestamp.txt";
        require(inspect_host_file(*config, runtime, inspection)["last_modified_utc"] ==
                    "1970-01-01T00:00:00.736Z",
                "native timestamp rounds to JavaScript milliseconds");
#ifdef _WIN32
        require(path_text(resolve_host_path("one/../two.txt", path_from_utf8("C:\\base"))) ==
                    "C:\\base\\two.txt",
                "Windows host path normalization");
        require(path_text(resolve_host_path("\\root.txt", path_from_utf8("C:\\base"))) == "\\root.txt",
                "rooted Windows host path contract");
        bool rejected = false;
        try {
            (void)resolve_host_path("$env:TEMP/file.txt", root);
        } catch (const Error&) {
            rejected = true;
        }
        require(rejected, "literal host path does not interpolate shell syntax");
        write_file(root / "valid.ps1", "Write-Output 'valid'\n");
        inspection.path = "valid.ps1";
        inspected = inspect_host_file(*config, runtime, inspection);
        require(inspected["powershell_syntax"]["parse_ok"] == true, inspected.dump().c_str());
        write_file(root / "broken.ps1", "function Broken {\n");
        inspection.path = "broken.ps1";
        inspected = inspect_host_file(*config, runtime, inspection);
        require(inspected["syntax_invalid"] == true && inspected["likely_corrupted_on_disk"] == false,
                "PowerShell syntax is distinct from byte corruption");
#endif
        fs::remove_all(root);
        std::cout << "Native/ripgrep search, bounded patterns, cancellation and exact file inspection checks "
                     "passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
