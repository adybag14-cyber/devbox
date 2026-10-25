#include "devbox/bundled_tools.hpp"
#include "bundled_ripgrep_identity.hpp"
#include <array>
#include <openssl/evp.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif
namespace devbox {
Json bundled_ripgrep_identity() {
    return {{"name", "ripgrep"},
            {"version", bundled_rg_version},
            {"source", bundled_rg_source},
            {"sha256", bundled_rg_sha256},
            {"bytes", bundled_rg_bytes},
            {"file", bundled_rg_file},
            {"pcre2", false},
            {"optional", true},
            {"compiledIdentity", bundled_rg_present},
            {"releaseQualified", bundled_rg_qualified}};
}
std::optional<BundledExecutable> verified_bundled_ripgrep(const fs::path& directory, const Cancel& cancel) {
    if constexpr (!bundled_rg_present) {
        return std::nullopt;
    } else {
        if (!directory.is_absolute())
            return std::nullopt;
        if (cancel)
            cancel->check();
        const auto path = directory / bundled_rg_file;
        std::error_code ec;
        const auto state = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory || state.type() == fs::file_type::not_found)
            return std::nullopt;
        if (ec || state.type() != fs::file_type::regular)
            throw Error("BUNDLED_RG_UNSAFE_FILE");
#ifdef _WIN32
        NativeHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER size{};
        if (!file ||
            !GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes,
                                          sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !GetFileSizeEx(file.get(), &size) || size.QuadPart != static_cast<LONGLONG>(bundled_rg_bytes))
            throw Error("BUNDLED_RG_IDENTITY_MISMATCH");
#else
        NativeHandle file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat before{};
        if (!file || ::fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
            (before.st_uid != ::geteuid() && before.st_uid != 0) || (before.st_mode & 022) != 0 ||
            before.st_size != static_cast<off_t>(bundled_rg_bytes))
            throw Error("BUNDLED_RG_IDENTITY_MISMATCH");
#endif
        const auto context =
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
            throw Error("BUNDLED_RG_HASH_FAILED");
        std::array<unsigned char, 8192> buffer{};
        std::uint64_t remaining = bundled_rg_bytes;
        while (remaining) {
            if (cancel)
                cancel->check();
            const auto wanted = static_cast<unsigned>(std::min<std::uint64_t>(remaining, buffer.size()));
#ifdef _WIN32
            DWORD count = 0;
            if (!ReadFile(file.get(), buffer.data(), wanted, &count, nullptr) || !count)
                throw Error("BUNDLED_RG_READ_FAILED");
#else
            const auto count = ::read(file.get(), buffer.data(), wanted);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw Error("BUNDLED_RG_READ_FAILED");
#endif
            if (static_cast<std::uint64_t>(count) > remaining ||
                EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
                throw Error("BUNDLED_RG_HASH_FAILED");
            remaining -= static_cast<std::uint64_t>(count);
        }
        std::array<std::uint8_t, 32> digest{};
        unsigned count = 0;
        if (EVP_DigestFinal_ex(context.get(), digest.data(), &count) != 1 || count != digest.size() ||
            hex(digest) != bundled_rg_sha256)
            throw Error("BUNDLED_RG_IDENTITY_MISMATCH");
#ifndef _WIN32
        struct stat after{};
        if (::fstat(file.get(), &after) != 0 || before.st_size != after.st_size ||
            before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime ||
            ::access(path.c_str(), X_OK) != 0)
            throw Error("BUNDLED_RG_CHANGED_OR_NOT_EXECUTABLE");
#endif
        return BundledExecutable{fs::canonical(path), std::move(file)};
    }
}
std::optional<fs::path> host_ripgrep(const Environment& environment) {
    auto clean = environment;
    std::string search;
    for (auto it = clean.begin(); it != clean.end();) {
        if (
#ifdef _WIN32
            lower(it->first) == "path"
#else
            it->first == "PATH"
#endif
        ) {
            search = it->second;
            it = clean.erase(it);
        } else
            ++it;
    }
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::vector<std::string> directories;
    for (auto directory : split(search, separator)) {
        if (directory.size() > 1 && directory.front() == '"' && directory.back() == '"')
            directory = directory.substr(1, directory.size() - 2);
        // An empty/relative PATH component must not turn a user workspace into
        // an executable fallback directory.
        if (path_from_utf8(directory).is_absolute())
            directories.push_back(directory);
    }
    clean["PATH"] = join(directories, std::string(1, separator));
    if (directories.empty())
        return std::nullopt;
    return find_program("rg", &clean);
}
} // namespace devbox
