#include "devbox/native.hpp"
#include "devbox/search.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace devbox {
namespace {
std::string upper(std::string value) {
    for (auto& c : value)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return value;
}
std::size_t count(std::string_view value, std::string_view needle) {
    std::size_t result = 0, offset = 0;
    while ((offset = value.find(needle, offset)) != value.npos) {
        ++result;
        offset += needle.size();
    }
    return result;
}
std::string preview(std::string_view text) {
    std::size_t index = 0, characters = 0;
    while (index < text.size() && characters < 400) {
        const auto c = static_cast<unsigned char>(text[index]);
        index += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        ++characters;
    }
    return std::string(text.substr(0, std::min(index, text.size())));
}
std::optional<std::string> bom(std::string_view bytes) {
    if (bytes.starts_with("\xef\xbb\xbf"))
        return "utf8";
    if (bytes.starts_with("\xff\xfe"))
        return "utf16le";
    if (bytes.starts_with("\xfe\xff"))
        return "utf16be";
    return {};
}
std::optional<std::string> binary_magic(std::string_view bytes) {
    if (bytes.starts_with("MZ"))
        return "pe";
    if (bytes.starts_with("\x7f"
                          "ELF"))
        return "elf";
    if (bytes.size() >= 3 && bytes.starts_with("PK") && (bytes[2] == 3 || bytes[2] == 5 || bytes[2] == 7))
        return "zip";
    if (bytes.starts_with("\x1f\x8b"))
        return "gzip";
    if (bytes.starts_with("\x37\x7a\xbc\xaf\x27\x1c"))
        return "7z";
    if (bytes.starts_with(std::string_view("\x52\x61\x72\x21\x1a\x07\x00", 7)))
        return "rar";
    return {};
}
std::string line_endings(const std::string& text) {
    const bool crlf = text.find("\r\n") != text.npos;
    const auto rest = replace_all(text, "\r\n", "");
    const bool lf = rest.find('\n') != rest.npos, cr = rest.find('\r') != rest.npos;
    const auto kinds = static_cast<int>(crlf) + static_cast<int>(lf) + static_cast<int>(cr);
    if (kinds > 1)
        return "mixed";
    if (!kinds)
        return "none";
    return crlf ? "crlf" : lf ? "lf" : "cr";
}
Json syntax_failure(const std::string& message) {
    return Json{
        {"parse_ok", false},
        {"error_count", 1},
        {"errors", Json::array({Json{
                       {"message", message}, {"line", nullptr}, {"column", nullptr}, {"text", nullptr}}})}};
}
Json modification_time(const fs::path& path) {
#ifdef _WIN32
    NativeHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file)
        return nullptr;
    FILETIME modified{};
    if (!GetFileTime(file.get(), nullptr, nullptr, &modified))
        return nullptr;
    const auto ticks = (static_cast<std::uint64_t>(modified.dwHighDateTime) << 32) | modified.dwLowDateTime;
    const auto relative = static_cast<std::int64_t>(ticks) - 116444736000000000LL;
    return utc_from_millis((relative >= 0 ? relative + 5000 : relative - 5000) / 10000);
#else
    struct stat metadata{};
    if (::stat(path.c_str(), &metadata) != 0)
        return nullptr;
#ifdef __APPLE__
    const auto modified = metadata.st_mtimespec;
#else
    const auto modified = metadata.st_mtim;
#endif
    const auto nanos = static_cast<std::int64_t>(modified.tv_sec) * 1000000000LL + modified.tv_nsec;
    return utc_from_millis((nanos >= 0 ? nanos + 500000 : nanos - 500000) / 1000000);
#endif
}
Json powershell_syntax(const RuntimeExecutor& runtime, const fs::path& path, const fs::path& workdir,
                       const Cancel& cancel) {
    const auto escaped = replace_all(path_text(path), "'", "''");
    ShellRequest request;
    request.command =
        "$tokens=$null;$errors=$null;[System.Management.Automation.Language.Parser]::ParseFile('" + escaped +
        "',[ref]$tokens,[ref]$errors)|Out-Null;$r=@{parse_ok=(@($errors).Count -eq "
        "0);error_count=@($errors).Count;errors=@(@($errors)|Select-Object -First 8|ForEach-Object { "
        "@{message=$_.Message;line=$_.Extent.StartLineNumber;column=$_.Extent.StartColumnNumber;text=$_."
        "Extent.Text}})};[Console]::Out.Write(($r|ConvertTo-Json -Compress -Depth 6))";
    request.working_dir = workdir;
    request.timeout = Millis(15000);
    request.max_capture_chars = 65536;
    try {
        const auto output = runtime.run_inspection_shell(request, cancel);
        try {
            return Json::parse(trim(output.stdout_text));
        } catch (const std::exception& e) {
            return syntax_failure(std::string("PowerShell parser returned invalid JSON: ") + e.what());
        }
    } catch (const std::exception& e) {
        return syntax_failure(e.what());
    }
}
} // namespace
fs::path resolve_host_path(std::string_view requested, const fs::path& workdir) {
    const auto text = trim(requested);
    if (text.starts_with('~')) {
        const auto home = path_from_utf8(env_or("USERPROFILE", env_or("HOME", path_text(workdir))));
        const auto first = text.find_first_not_of("/\\", 1);
        return first == text.npos ? home : home / path_from_utf8(text.substr(first));
    }
    const auto path = path_from_utf8(text);
    return path.is_absolute() ? path : workdir / path;
}
Json inspect_host_file(const Config& config, const RuntimeExecutor& runtime,
                       const InspectFileRequest& request, const Cancel& cancel) {
    if (cancel)
        cancel->check();
    const auto resolved =
        request.resolved_path.value_or(resolve_host_path(request.path, request.working_dir));
    const auto extension = lower(path_text(resolved.extension()));
    Json info{{"requested_path", request.path},
              {"resolved_path", path_text(resolved)},
              {"extension", extension},
              {"exists", false},
              {"is_file", false},
              {"likely_corrupted_on_disk", false},
              {"syntax_invalid", false},
              {"observations", Json::array()},
              {"repair_hints", Json::array()}};
    std::error_code ec;
    const auto metadata = fs::status(resolved, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(metadata))) {
        info["observations"].push_back("The path does not exist on disk.");
        info["repair_hints"].push_back("Verify the path or recreate the file before retrying the command.");
        return info;
    }
    if (ec)
        throw Error("inspect " + path_text(resolved) + ": " + ec.message());
    info["exists"] = true;
    const bool regular = fs::is_regular_file(metadata);
    info["is_file"] = regular;
    std::uint64_t size = 0;
    if (regular) {
        size = fs::file_size(resolved, ec);
        if (ec)
            throw Error(ec.message());
    }
#ifndef _WIN32
    else {
        struct stat data{};
        if (::stat(resolved.c_str(), &data) == 0)
            size = static_cast<std::uint64_t>(data.st_size);
    }
#endif
    info["size_bytes"] = size;
    info["last_modified_utc"] = modification_time(resolved);
    if (!regular) {
        info["observations"].push_back("The path exists but is not a regular file.");
        return info;
    }
    const auto bytes = read_file_range(resolved, 0, std::max<std::size_t>(1, request.max_bytes));
    const auto prefix = bom(bytes), format = binary_magic(bytes);
    info["sampled_bytes"] = bytes.size();
    info["sha256"] = size <= 8 * 1024 * 1024 ? Json(sha256_file(resolved, 8 * 1024 * 1024)) : Json(nullptr);
    info["bom"] = prefix ? Json(*prefix) : Json(nullptr);
    info["binary_format"] = format ? Json(*format) : Json(nullptr);
    info["text_inspection_skipped"] = format.has_value();
    if (format) {
        info["utf8_valid"] = nullptr;
        info["observations"].push_back("Skipped text-corruption tests because the file has " +
                                       upper(*format) + " binary magic.");
        return info;
    }
    const auto decoded = sanitize_utf8(bytes);
    const bool valid = decoded == bytes;
    const auto nulls = count(bytes, std::string_view("\0", 1)), replacements = count(decoded, "�");
    const std::array<std::string_view, 9> markers = {"â€”", "â€“", "â€œ", "â€�", "â€˜",
                                                     "â€™", "â€¦", "â€¢", "ðŸ"};
    std::size_t mojibake = 0;
    std::vector<std::string> found;
    for (const auto marker : markers) {
        const auto hits = count(decoded, marker);
        if (hits) {
            found.emplace_back(marker);
            mojibake += hits;
        }
    }
    info["utf8_valid"] = valid;
    info["null_byte_count"] = nulls;
    info["replacement_character_count"] = replacements;
    info["suspicious_mojibake_count"] = mojibake;
    info["suspicious_mojibake_markers"] = found;
    info["line_endings"] = line_endings(decoded);
    info["preview"] = preview(decoded);
    if (prefix)
        info["observations"].push_back("The file starts with a " + upper(*prefix) + " BOM.");
    if (!valid)
        info["observations"].push_back("The sampled bytes are not valid UTF-8.");
    if (nulls)
        info["observations"].push_back("The sampled bytes contain " + std::to_string(nulls) +
                                       " NUL byte(s), which is unusual for a text source file.");
    if (mojibake)
        info["observations"].push_back("The sampled text contains " + std::to_string(mojibake) +
                                       " suspicious mojibake marker(s): " + join(found, ", ") + ".");
    if (config.platform.is_windows && (extension == ".ps1" || extension == ".psm1" || extension == ".psd1")) {
        const auto syntax = powershell_syntax(runtime, resolved, request.working_dir, cancel);
        info["powershell_syntax"] = syntax;
        const bool invalid = syntax.value("parse_ok", true) == false;
        info["syntax_invalid"] = invalid;
        if (invalid)
            info["observations"].push_back("PowerShell reported " +
                                           std::to_string(json_uint(syntax, "error_count", 1)) +
                                           " parse error(s) for this file.");
    }
    const bool corrupted = !valid || nulls || mojibake || replacements;
    info["likely_corrupted_on_disk"] = corrupted;
    if (corrupted) {
        info["repair_hints"].push_back("Read the exact bytes with windows_host_read_large_file before "
                                       "editing so you do not lose evidence of the corruption.");
        info["repair_hints"].push_back(
            "Rewrite the file from a clean UTF-8 or exact-byte payload with windows_host_write_large_file, "
            "then rerun the original host command.");
    } else if (json_bool(info, "syntax_invalid"))
        info["repair_hints"].push_back("The file appears to be syntactically invalid but not obviously "
                                       "byte-corrupted; inspect the script text and fix the source logic.");
    return info;
}
} // namespace devbox
