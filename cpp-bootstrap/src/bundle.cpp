#include "devbox/bundle.hpp"
#include "devbox/native.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <openssl/evp.h>
#include <set>
#include <zlib.h>

namespace devbox::bundle {
namespace {
constexpr std::size_t chunk = 65536;
constexpr std::string_view magic = "DEVBOX-OFFLINE01";
static_assert(magic.size() == 16);
bool digest_text(std::string_view value, std::size_t length = 64) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void checked_read(std::ifstream& input, char* data, std::size_t size) {
    input.read(data, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size))
        throw Error("OFFLINE_TRUNCATED_PAYLOAD");
}
void checked_write(std::ofstream& output, const char* data, std::size_t size) {
    output.write(data, static_cast<std::streamsize>(size));
    if (!output)
        throw Error("OFFLINE_WRITE_FAILED");
}
void copy_stream(std::ifstream& input, std::ofstream& output) {
    std::array<char, chunk> bytes{};
    while (input) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        checked_write(output, bytes.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof())
        throw Error("OFFLINE_READ_FAILED");
}
std::uint64_t compress_file(const fs::path& source, std::ofstream& output) {
    std::ifstream input(source, std::ios::binary);
    if (!input)
        throw Error("OFFLINE_INPUT_UNREADABLE");
    z_stream stream{};
    if (deflateInit(&stream, Z_BEST_SPEED) != Z_OK)
        throw Error("OFFLINE_COMPRESSION_INIT");
    ScopeExit cleanup([&] { deflateEnd(&stream); });
    std::array<char, chunk> in{}, out{};
    std::uint64_t written = 0;
    for (;;) {
        input.read(in.data(), static_cast<std::streamsize>(in.size()));
        if (!input && !input.eof())
            throw Error("OFFLINE_INPUT_READ");
        const bool final = input.eof();
        stream.next_in = reinterpret_cast<Bytef*>(in.data());
        stream.avail_in = static_cast<uInt>(input.gcount());
        do {
            stream.next_out = reinterpret_cast<Bytef*>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());
            const auto status = deflate(&stream, final ? Z_FINISH : Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END)
                throw Error("OFFLINE_COMPRESSION_FAILED");
            const auto count = out.size() - stream.avail_out;
            checked_write(output, out.data(), count);
            written += count;
            if (status == Z_STREAM_END)
                return written;
        } while (stream.avail_in || !stream.avail_out || final);
    }
}
void expand_file(std::ifstream& input, const Json& entry, const fs::path& destination) {
    const auto compressed = entry.at("compressedBytes").get<std::uint64_t>();
    const auto expected = entry.at("bytes").get<std::uint64_t>();
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        throw Error("OFFLINE_OUTPUT_UNWRITABLE");
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        throw Error("OFFLINE_DECOMPRESSION_INIT");
    ScopeExit cleanup([&] { inflateEnd(&stream); });
    std::array<char, chunk> in{}, out{};
    std::uint64_t remaining = compressed, written = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (!stream.avail_in) {
            if (!remaining)
                throw Error("OFFLINE_TRUNCATED_STREAM");
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, in.size()));
            checked_read(input, in.data(), count);
            remaining -= count;
            stream.next_in = reinterpret_cast<Bytef*>(in.data());
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = reinterpret_cast<Bytef*>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());
        const auto before = stream.avail_in;
        status = inflate(&stream, Z_NO_FLUSH);
        const auto count = out.size() - stream.avail_out;
        if ((status != Z_OK && status != Z_STREAM_END) ||
            (status == Z_OK && !count && before == stream.avail_in))
            throw Error("OFFLINE_CORRUPT_STREAM");
        if (count > expected - written)
            throw Error("OFFLINE_EXPANSION_LIMIT");
        checked_write(output, out.data(), count);
        written += count;
    }
    if (remaining || stream.avail_in || written != expected)
        throw Error("OFFLINE_STREAM_SIZE_MISMATCH");
    output.close();
    if (!output || sha256_file(destination, maximum_file) != json_string(entry, "sha256"))
        throw Error("OFFLINE_FILE_DIGEST_MISMATCH");
#ifndef _WIN32
    fs::permissions(destination,
                    fs::perms::owner_read | fs::perms::owner_write |
                        (json_bool(entry, "executable") ? fs::perms::owner_exec : fs::perms::none));
#endif
}
} // namespace
bool safe_name(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '.' || name.back() == '.')
        return false;
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                   c == '_' || c == '.';
        }))
        return false;
    const auto stem = lower(std::string(name.substr(0, name.find('.'))));
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" && stem != "clock$" &&
           !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' &&
             stem[3] <= '9');
}
std::string region_hash(const fs::path& file, std::uint64_t offset, std::uint64_t size) {
    const auto total = fs::file_size(file);
    if (size > maximum_payload || offset > total || size > total - offset)
        throw Error("OFFLINE_PAYLOAD_BOUNDS");
    std::ifstream input(file, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    const auto context =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw Error("OFFLINE_HASH_INIT");
    std::array<char, chunk> bytes{};
    while (size) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, bytes.size()));
        checked_read(input, bytes.data(), count);
        if (EVP_DigestUpdate(context.get(), bytes.data(), count) != 1)
            throw Error("OFFLINE_HASH_FAILED");
        size -= count;
    }
    std::array<std::uint8_t, 32> digest{};
    unsigned count = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &count) != 1 || count != digest.size())
        throw Error("OFFLINE_HASH_FAILED");
    return hex(digest);
}
void validate(const Json& manifest) {
    if (json_uint(manifest, "schema") != 1 || json_string(manifest, "format") != "devbox-offline-zlib" ||
        !digest_text(json_string(manifest, "source"), 40) ||
        !digest_text(json_string(manifest, "tree"), 40) ||
        !digest_text(json_string(manifest, "payloadSha256")) || !manifest.contains("files") ||
        !manifest["files"].is_array() || manifest["files"].empty() || manifest["files"].size() > 32 ||
        json_string(manifest, "target").empty())
        throw Error("OFFLINE_MANIFEST_INVALID");
    std::set<std::string> names;
    std::uint64_t next = 0, expanded = 0;
    for (const auto& entry : manifest["files"]) {
        const auto name = json_string(entry, "name");
        if (!safe_name(name) || !names.insert(lower(name)).second ||
            !digest_text(json_string(entry, "sha256")) || !entry.at("executable").is_boolean() ||
            !entry.at("bytes").is_number_unsigned() || !entry.at("compressedBytes").is_number_unsigned() ||
            !entry.at("offset").is_number_unsigned())
            throw Error("OFFLINE_ENTRY_INVALID");
        const auto bytes = entry.at("bytes").get<std::uint64_t>();
        const auto compressed = entry.at("compressedBytes").get<std::uint64_t>();
        if (bytes > maximum_file || bytes > maximum_payload - expanded || !compressed ||
            compressed > maximum_payload - next || entry.at("offset").get<std::uint64_t>() != next)
            throw Error("OFFLINE_ENTRY_BOUNDS");
        expanded += bytes;
        next += compressed;
    }
    if (!manifest.at("payloadBytes").is_number_unsigned() ||
        manifest.at("payloadBytes").get<std::uint64_t>() != next ||
        manifest.at("expandedBytes").get<std::uint64_t>() != expanded)
        throw Error("OFFLINE_PAYLOAD_SIZE_MISMATCH");
}
Json pack(const fs::path& package, const fs::path& payload, std::string workflow) {
    const auto build = read_json(package / "build-manifest.json", 4 * 1024 * 1024);
    if (json_string(build, "implementation") != "cpp" || !json_bool(build, "releaseReady") ||
        json_bool(build.at("build"), "sourceDirty", true) || json_bool(build.at("build"), "sanitizers", true))
        throw Error("OFFLINE_UNQUALIFIED_SOURCE");
    if (workflow.empty() ||
        !std::all_of(workflow.begin(), workflow.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw Error("OFFLINE_HOSTED_WORKFLOW_REQUIRED");
    if (fs::exists(payload))
        throw Error("OFFLINE_OUTPUT_EXISTS");
    std::ofstream output(payload, std::ios::binary);
    Json manifest{{"schema", 1},
                  {"format", "devbox-offline-zlib"},
                  {"source", build.at("gitSha")},
                  {"tree", build.at("sourceTree")},
                  {"target", build.at("target")},
                  {"version", build.at("setupVersion")},
                  {"workflowRunId", std::move(workflow)},
                  {"files", Json::array()}};
    std::vector<std::pair<fs::path, bool>> files;
    std::set<std::string> binaries;
    for (const auto& entry : build.at("binaries")) {
        const auto name = json_string(entry, "name");
        const auto target = json_string(build, "target");
        const auto expected = (target.starts_with("android-") ? "" : "bin/") + name +
                              (target.starts_with("windows-") ? ".exe" : "");
        if ((name != "devbox-mcp" && name != "devbox-setup" && name != "devbox-tui") ||
            !binaries.insert(name).second || json_string(entry, "file") != expected)
            throw Error("OFFLINE_BINARY_PATH_INVALID");
        const auto source = package / path_from_utf8(json_string(entry, "file"));
        if (fs::is_symlink(source) || !fs::is_regular_file(source) ||
            fs::file_size(source) != json_uint(entry, "bytes") ||
            sha256_file(source, maximum_file) != json_string(entry, "sha256"))
            throw Error("OFFLINE_BINARY_IDENTITY_MISMATCH");
        files.emplace_back(source, true);
    }
    if (!binaries.contains("devbox-mcp"))
        throw Error("OFFLINE_RUNTIME_REQUIRED");
    for (const auto* name : {"build-manifest.json", "THIRD_PARTY_NOTICES.txt", "dependency-inventory.json",
                             "sbom.spdx.json", "OFFLINE.md", "CORE_SERVER.md"})
        files.emplace_back(package / name, false);
    std::uint64_t offset = 0, expanded = 0;
    for (const auto& [source, executable] : files) {
        const auto name = path_text(source.filename());
        if (!safe_name(name) || fs::is_symlink(source) || !fs::is_regular_file(source) ||
            fs::file_size(source) > maximum_file)
            throw Error("OFFLINE_INPUT_INVALID");
        const auto hash = sha256_file(source, maximum_file);
        const auto count = fs::file_size(source);
        const auto compressed = compress_file(source, output);
        if (sha256_file(source, maximum_file) != hash || fs::file_size(source) != count)
            throw Error("OFFLINE_INPUT_CHANGED");
        manifest["files"].push_back({{"name", name},
                                     {"bytes", count},
                                     {"compressedBytes", compressed},
                                     {"offset", offset},
                                     {"sha256", hash},
                                     {"executable", executable}});
        offset += compressed;
        expanded += count;
        if (offset > maximum_payload || expanded > maximum_payload)
            throw Error("OFFLINE_PACKAGE_TOO_LARGE");
    }
    output.close();
    if (!output)
        throw Error("OFFLINE_WRITE_FAILED");
    manifest["payloadBytes"] = offset;
    manifest["expandedBytes"] = expanded;
    manifest["payloadSha256"] = region_hash(payload, 0, offset);
    validate(manifest);
    return manifest;
}
void extract(const fs::path& payload, std::uint64_t offset, const Json& manifest, const fs::path& directory) {
    validate(manifest);
    if (fs::exists(directory))
        throw Error("OFFLINE_DESTINATION_EXISTS");
    if (region_hash(payload, offset, json_uint(manifest, "payloadBytes")) !=
        json_string(manifest, "payloadSha256"))
        throw Error("OFFLINE_PAYLOAD_DIGEST_MISMATCH");
    ensure_private_state_directory(directory, true);
    ScopeExit rollback([&] {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    });
    std::ifstream input(payload, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    for (const auto& entry : manifest["files"])
        expand_file(input, entry, directory / path_from_utf8(json_string(entry, "name")));
    rollback.disarm();
}
void attach(const fs::path& executable, const fs::path& payload, const fs::path& output) {
    const auto size = fs::file_size(payload);
    if (!size || size > maximum_payload || fs::exists(output))
        throw Error("OFFLINE_ATTACH_BOUNDS_OR_EXISTS");
    std::ifstream stub(executable, std::ios::binary), body(payload, std::ios::binary);
    if (!stub || !body)
        throw Error("OFFLINE_INPUT_UNREADABLE");
    std::ofstream destination(output, std::ios::binary);
    copy_stream(stub, destination);
    copy_stream(body, destination);
    std::array<char, 8> trailer{};
    for (unsigned i = 0; i < trailer.size(); ++i)
        trailer[i] = static_cast<char>((size >> (i * 8)) & 255);
    checked_write(destination, trailer.data(), trailer.size());
    checked_write(destination, magic.data(), magic.size());
    destination.close();
    if (!destination)
        throw Error("OFFLINE_WRITE_FAILED");
#ifndef _WIN32
    fs::permissions(output, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
#endif
}
std::uint64_t payload_offset(const fs::path& executable, std::uint64_t expected) {
    const auto size = fs::file_size(executable);
    if (!expected || expected > maximum_payload || size < expected + 24)
        throw Error("OFFLINE_TRAILER_BOUNDS");
    std::ifstream input(executable, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(size - 24));
    std::array<char, 24> trailer{};
    checked_read(input, trailer.data(), trailer.size());
    std::uint64_t actual = 0;
    for (unsigned i = 0; i < 8; ++i)
        actual |= static_cast<std::uint64_t>(static_cast<unsigned char>(trailer[i])) << (i * 8);
    if (actual != expected || std::string_view(trailer.data() + 8, 16) != magic)
        throw Error("OFFLINE_TRAILER_INVALID");
    return size - 24 - expected;
}
} // namespace devbox::bundle
