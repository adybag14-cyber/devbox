#include "devbox/storage.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <openssl/evp.h>
#include <semaphore>
#include <set>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace devbox {
namespace {
class DiskFile {
    NativeHandle handle_;

  public:
    bool missing = false;
    explicit DiskFile(const fs::path& path, bool create_new = false) {
#ifdef _WIN32
        handle_.reset(CreateFileW(path.c_str(), create_new ? GENERIC_WRITE | GENERIC_READ : GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  create_new ? CREATE_NEW : OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!handle_) {
            const auto error = GetLastError();
            if (!create_new && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
                missing = true;
                return;
            }
            throw Error(windows_error(error));
        }
        if (GetFileType(handle_.get()) != FILE_TYPE_DISK ||
            (info().dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            throw Error("Target is not a regular file");
#else
        handle_.reset(::open(
            path.c_str(),
            create_new ? O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC : O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0600));
        if (!handle_) {
            if (!create_new && errno == ENOENT) {
                missing = true;
                return;
            }
            throw Error(std::strerror(errno));
        }
        if (!S_ISREG(info().st_mode))
            throw Error("Target is not a regular file");
#endif
    }
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info() const {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_.get(), &info))
            throw Error(windows_error());
        return info;
    }
#else
    struct stat info() const {
        struct stat info{};
        if (::fstat(handle_.get(), &info))
            throw Error(std::strerror(errno));
        return info;
    }
#endif
    std::uint64_t size() const {
        const auto value = info();
#ifdef _WIN32
        return (static_cast<std::uint64_t>(value.nFileSizeHigh) << 32) | value.nFileSizeLow;
#else
        return static_cast<std::uint64_t>(value.st_size);
#endif
    }
    std::size_t read(std::span<char> buffer) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(handle_.get(), buffer.data(),
                      static_cast<DWORD>(std::min<std::size_t>(buffer.size(), MAXDWORD)), &count, nullptr))
            throw Error(windows_error());
        return count;
#else
        ssize_t count;
        do {
            count = ::read(handle_.get(), buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            throw Error(std::strerror(errno));
        return static_cast<std::size_t>(count);
#endif
    }
    void seek(std::uint64_t offset) {
        if (offset > static_cast<std::uint64_t>(INT64_MAX))
            throw Error("File offset exceeds supported range");
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN))
            throw Error(windows_error());
#else
        if (::lseek(handle_.get(), static_cast<off_t>(offset), SEEK_SET) < 0)
            throw Error(std::strerror(errno));
#endif
    }
    void write(std::string_view bytes) {
        while (!bytes.empty()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(handle_.get(), bytes.data(),
                           static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536)), &count, nullptr))
                throw Error(windows_error());
#else
            ssize_t count;
            do {
                count = ::write(handle_.get(), bytes.data(), std::min<std::size_t>(bytes.size(), 65536));
            } while (count < 0 && errno == EINTR);
            if (count < 0)
                throw Error(std::strerror(errno));
#endif
            if (!count)
                throw Error("File write made no progress");
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }
    void sync() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_.get()))
            throw Error(windows_error());
#else
        if (::fsync(handle_.get()))
            throw Error(std::strerror(errno));
#endif
    }
    void preserve_permissions(const DiskFile& previous) {
#ifndef _WIN32
        if (::fchmod(handle_.get(), previous.info().st_mode & 07777))
            throw Error(std::strerror(errno));
#else
        (void)previous;
#endif
    }
    void reject_readonly_or_alias() const {
        const auto value = info();
#ifdef _WIN32
        if (value.nNumberOfLinks > 1)
            throw Error("Atomic replacement of a hard-linked target is unsupported");
        if (value.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            throw Error("Target is read-only");
#else
        if (value.st_nlink > 1)
            throw Error("Atomic replacement of a hard-linked target is unsupported");
        if (!(value.st_mode & 0222))
            throw Error("Target is read-only");
#endif
    }
    void close() {
        handle_.reset();
    }
};
std::string hash_stream(DiskFile& file, std::uint64_t limit, std::uint64_t& count) {
    auto* digest = EVP_MD_CTX_new();
    if (!digest)
        throw Error("Cannot allocate SHA-256 state");
    ScopeExit release([&] { EVP_MD_CTX_free(digest); });
    if (EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) != 1)
        throw Error("Cannot initialize SHA-256");
    std::array<char, 65536> buffer{};
    count = 0;
    while (count < limit) {
        const auto n = file.read(std::span(buffer).first(
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), limit - count))));
        if (!n)
            break;
        if (EVP_DigestUpdate(digest, buffer.data(), n) != 1)
            throw Error("Cannot compute SHA-256");
        count += n;
    }
    std::array<std::uint8_t, 32> bytes{};
    unsigned length = 0;
    if (EVP_DigestFinal_ex(digest, bytes.data(), &length) != 1 || length != bytes.size())
        throw Error("Cannot finish SHA-256");
    return hex(bytes);
}
std::string read_exact(DiskFile& file, std::size_t size) {
    std::string result(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = file.read(std::span(result).subspan(offset));
        if (!count)
            throw Error("File changed while reading requested bytes");
        offset += count;
    }
    return result;
}
void private_directory(const fs::path& path) {
#ifdef _WIN32
    std::error_code ec;
    fs::create_directory(path, ec);
    if (ec && ec != std::errc::file_exists)
        throw std::system_error(ec);
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error("Atomic lock directory must be a private ordinary directory");
#else
    if (::mkdir(path.c_str(), 0700) && errno != EEXIST)
        throw Error(std::strerror(errno));
    struct stat info{};
    if (::lstat(path.c_str(), &info) || !S_ISDIR(info.st_mode))
        throw Error("Atomic lock directory must be a private ordinary directory");
    if (info.st_uid != ::geteuid() || info.st_mode & 0077)
        throw Error("Atomic lock directory must be owned by this account with mode 0700");
#endif
}
fs::path atomic_lock_root() {
#ifdef _WIN32
    const auto value = environment("LOCALAPPDATA");
#else
    const auto value = environment("HOME");
#endif
    if (!value)
        throw Error("Account profile is required for atomic locks");
    const auto profile = path_from_utf8(*value);
    if (!profile.is_absolute() || !fs::is_directory(profile))
        throw Error("Atomic locks require an absolute account profile directory");
#ifndef _WIN32
    struct stat info{};
    if (::stat(profile.c_str(), &info) || info.st_uid != ::geteuid() || (info.st_mode & 0022))
        throw Error("Atomic lock profile must belong to this account and not be writable by other accounts");
#endif
    const auto parent = profile / ".devbox";
    private_directory(parent);
    const auto root = parent / "atomic-locks-v2";
    private_directory(root);
    return root;
}
bool append_replayed(const fs::path& path, std::string_view payload, const Preconditions& expected) {
    if (!expected.offset || *expected.offset > UINT64_MAX - payload.size())
        return false;
    DiskFile file(path);
    if (file.missing || file.size() != *expected.offset + payload.size())
        return false;
    if (expected.sha256) {
        if (*expected.sha256 == "missing") {
            if (*expected.offset)
                return false;
        } else {
            std::uint64_t count = 0;
            if (hash_stream(file, *expected.offset, count) != lower(*expected.sha256) ||
                count != *expected.offset)
                return false;
        }
    }
    file.seek(*expected.offset);
    return read_exact(file, payload.size()) == payload;
}
void replace_staged(const fs::path& source, const fs::path& target, bool existed) {
#ifdef _WIN32
    const bool success =
        existed ? ReplaceFileW(target.c_str(), source.c_str(), nullptr, 0, nullptr, nullptr) != FALSE
                : MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success)
        throw Error(windows_error());
#else
    (void)existed;
    fs::rename(source, target);
    NativeHandle directory(::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory || ::fsync(directory.get()))
        throw Error(std::strerror(errno));
#endif
}
std::string normalize_hash(std::string value) {
    value = lower(trim(value));
    if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw Error("expected_sha256 must be a 64-character SHA-256 hex string.");
    return value;
}
std::vector<std::string> task_ids(const fs::path& root) {
    std::vector<std::string> ids;
    std::error_code ec;
    fs::directory_iterator iterator(root, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return ids;
    if (ec)
        throw std::system_error(ec);
    for (const auto& entry : iterator) {
        const auto name = path_text(entry.path().filename());
        if (!name.ends_with(".json"))
            continue;
        const auto id = name.substr(0, name.size() - 5);
        try {
            validate_key(id);
        } catch (const Error&) {
            continue;
        }
        if (ids.size() >= 10000)
            throw Error("Task index exceeds its scan budget");
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}
} // namespace
Json FileState::json() const {
    return Json{{"exists", exists}, {"bytes", bytes}, {"sha256", sha256 ? Json(*sha256) : Json(nullptr)}};
}
Json WriteReceipt::json() const {
    return Json{
        {"path", path}, {"previous", previous.json()}, {"current", current.json()}, {"replayed", replayed}};
}
FileLock::FileLock(const fs::path& path, Millis timeout, const Cancel& cancel, bool private_file) {
#ifdef _WIN32
    handle_.reset(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr));
    if (!handle_)
        throw Error(windows_error());
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_.get(), &info))
        throw Error(windows_error());
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        (private_file && info.nNumberOfLinks != 1))
        throw Error("Lock file must be an ordinary unaliased file");
#else
    handle_.reset(::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!handle_)
        throw Error(std::strerror(errno));
    struct stat info{};
    if (::fstat(handle_.get(), &info))
        throw Error(std::strerror(errno));
    if (!S_ISREG(info.st_mode) ||
        (private_file && (info.st_uid != ::geteuid() || (info.st_mode & 0077) || info.st_nlink != 1)))
        throw Error("Atomic lock file must be private, account-owned and unaliased");
#endif
    const auto deadline = Clock::now() + timeout;
    while (true) {
        if (cancel)
            cancel->check();
#ifdef _WIN32
        OVERLAPPED range{};
        if (LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                       MAXDWORD, &range))
            break;
        const auto error = GetLastError();
        if (error != ERROR_LOCK_VIOLATION)
            throw Error(windows_error(error));
#else
        if (!::flock(handle_.get(), LOCK_EX | LOCK_NB))
            break;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
            throw Error(std::strerror(errno));
#endif
        if (Clock::now() >= deadline)
            throw Error("File lock deadline exceeded");
        if (cancel)
            cancel->wait_for(Millis(10));
        else
            std::this_thread::sleep_for(Millis(10));
    }
}
fs::path canonical_target(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    const bool exists = fs::exists(path);
    if (fs::is_symlink(status) && !exists)
        throw Error("Atomic writes reject dangling symlinks");
    auto target = exists ? path : path.parent_path().empty() ? fs::path(".") : path.parent_path();
#ifdef _WIN32
    NativeHandle handle(CreateFileW(target.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle)
        throw Error(windows_error());
    const auto size =
        GetFinalPathNameByHandleW(handle.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size)
        throw Error(windows_error());
    std::wstring name(size, L'\0');
    const auto n =
        GetFinalPathNameByHandleW(handle.get(), name.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= size)
        throw Error(windows_error());
    name.resize(n);
    target = fs::path(name);
#else
    target = fs::canonical(target);
#endif
    if (!exists) {
        if (path.filename().empty())
            throw Error("file name required");
        target /= path.filename();
    }
    return target;
}
std::size_t atomic_lock_stripe(const fs::path& resolved) {
    auto key = path_text(resolved);
#ifdef _WIN32
    auto native = resolved.wstring();
    const auto count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(),
                                     static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!count)
        throw Error(windows_error());
    std::wstring lowered(static_cast<std::size_t>(count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(), static_cast<int>(native.size()),
                       lowered.data(), count, nullptr, nullptr, 0))
        throw Error(windows_error());
    key = narrow(lowered);
#endif
    return std::stoul(sha256(key).substr(0, 2), nullptr, 16);
}
FileState file_state(const fs::path& path) {
    DiskFile file(path);
    if (file.missing)
        return {};
    const auto length = file.size();
    std::uint64_t count = 0;
    auto digest = hash_stream(file, length + (length < UINT64_MAX ? 1 : 0), count);
    if (count != length)
        throw Error("File changed while computing its version");
    return {true, count, std::move(digest)};
}
WriteReceipt atomic_write(const fs::path& path, std::string_view payload, bool append, bool create_dirs,
                          const Preconditions& expected) {
    static std::counting_semaphore<2> capacity(2);
    if (!capacity.try_acquire_for(Millis(5000)))
        throw Error("Atomic I/O capacity wait timed out");
    ScopeExit release([&] { capacity.release(); });
    if (create_dirs && !path.parent_path().empty())
        fs::create_directories(path.parent_path());
    const auto target = canonical_target(path);
    FileLock lock(atomic_lock_root() / (std::to_string(atomic_lock_stripe(target)) + ".lock"), Millis(5000),
                  {}, true);
    const auto previous = file_state(target);
    const auto replay = [&] { return WriteReceipt{path_text(path), previous, previous, true}; };
    if (expected.sha256) {
        if (*expected.sha256 != "missing" &&
            (expected.sha256->size() != 64 ||
             !std::all_of(expected.sha256->begin(), expected.sha256->end(), [](char c) {
                 return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
             })))
            throw Error("expected_file_sha256 must be a SHA-256 digest or missing");
        const auto matches =
            *expected.sha256 == "missing" ? !previous.exists : previous.sha256 == lower(*expected.sha256);
        if (!matches) {
            if ((append && append_replayed(target, payload, expected)) ||
                (!append && previous.sha256 == sha256(payload)))
                return replay();
            throw Error("File version conflict: current content differs from expected_file_sha256");
        }
    }
    if (expected.offset) {
        if (!append)
            throw Error("expected_offset_bytes requires append=true");
        if (previous.bytes != *expected.offset) {
            if (append_replayed(target, payload, expected))
                return replay();
            throw Error("File offset conflict: append was not applied");
        }
    }
    DiskFile prior(target);
    if (!prior.missing)
        prior.reject_readonly_or_alias();
    const auto staged = target.parent_path() / path_from_utf8(".devbox-write-" + uuid());
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove(staged, ec);
    });
    DiskFile output(staged, true);
    if (append && previous.exists) {
        std::array<char, 65536> buffer{};
        std::uint64_t remaining = previous.bytes;
        while (remaining) {
            const auto n = prior.read(std::span(buffer).first(
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining))));
            if (!n)
                throw Error("File changed while preparing atomic replacement");
            output.write(std::string_view(buffer.data(), n));
            remaining -= n;
        }
    }
    output.write(payload);
    if (!prior.missing)
        output.preserve_permissions(prior);
    output.sync();
    output.close();
    prior.close();
    const auto current = file_state(staged);
    if (file_state(target).sha256 != previous.sha256)
        throw Error("File changed while preparing atomic replacement");
    replace_staged(staged, target, previous.exists);
    cleanup.disarm();
    return {path_text(path), previous, current, false};
}
std::string read_text(const fs::path& path, std::size_t max_bytes) {
    DiskFile file(path);
    if (file.missing)
        throw Error("ENOENT: no such file or directory, open '" + path_text(path) + "'");
    const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(file.size(), std::max<std::size_t>(max_bytes, 1)));
    return sanitize_utf8(read_exact(file, count));
}
Json read_large(const fs::path& path, std::uint64_t offset, std::size_t max_bytes) {
    DiskFile file(path);
    if (file.missing)
        throw Error("ENOENT: no such file or directory, stat '" + path_text(path) + "'");
    const auto size = file.size(), actual = std::min(offset, size);
    const auto requested = std::max<std::size_t>(max_bytes, 1);
    file.seek(actual);
    const auto bytes =
        read_exact(file, static_cast<std::size_t>(std::min<std::uint64_t>(size - actual, requested)));
    return Json{{"path", path_text(path)},
                {"file_size", size},
                {"offset_bytes_requested", offset},
                {"offset_bytes", actual},
                {"bytes_requested", requested},
                {"bytes_returned", bytes.size()},
                {"next_offset_bytes", actual + bytes.size()},
                {"eof", actual + bytes.size() >= size},
                {"content_sha256", sha256(bytes)},
                {"content_base64", base64_encode(bytes)}};
}
Json write_large(const fs::path& path, std::string_view content_base64, bool append, bool create_dirs,
                 const std::optional<std::string>& expected_sha256) {
    std::string normalized;
    for (const auto c : content_base64)
        if (!std::isspace(static_cast<unsigned char>(c)))
            normalized += c;
    std::vector<std::uint8_t> payload;
    try {
        payload = base64_decode(normalized);
    } catch (...) {
        throw Error("content_base64 is not valid base64");
    }
    if (base64_encode(payload) != normalized)
        throw Error("content_base64 is not canonical base64");
    const auto hash = sha256(payload);
    const auto expected = expected_sha256 && !trim(*expected_sha256).empty()
                              ? std::optional(normalize_hash(*expected_sha256))
                              : std::nullopt;
    if (expected && *expected != hash)
        throw Error("Decoded payload SHA-256 did not match expected_sha256.");
    const auto receipt =
        atomic_write(path, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()),
                     append, create_dirs);
    bool verified;
    std::optional<std::string> file_hash;
    DiskFile file(path);
    const auto size = file.size();
    if (append) {
        file.seek(receipt.previous.bytes);
        const auto suffix = read_exact(file, payload.size());
        verified = size == receipt.previous.bytes + payload.size() && sha256(suffix) == hash;
    } else {
        file_hash = file_state(path).sha256;
        verified = size == payload.size() && file_hash == hash;
    }
    if (!verified)
        throw Error("Mirror verification failed after writing the payload.");
    return Json{{"path", path_text(path)},
                {"append", append},
                {"previous_file_size", receipt.previous.bytes},
                {"final_file_size", size},
                {"bytes_written", payload.size()},
                {"content_sha256", hash},
                {"verification_mode", append ? "suffix-bytes" : "whole-file-sha256"},
                {"verified", verified},
                {"expected_sha256_verified", expected ? Json(true) : Json(nullptr)},
                {"target_existed", receipt.previous.exists},
                {"file_sha256", file_hash ? Json(*file_hash) : Json(nullptr)}};
}
ProcessOutput list_files(const ListOptions& options, const Cancel& cancel) {
    const auto deadline = Clock::now() + std::max(options.timeout, Millis(1));
    std::set<std::string> excluded;
    for (const auto& value : options.exclude_directories)
        if (!trim(value).empty())
            excluded.insert(lower(trim(value)));
    const auto max_entries = std::max<std::size_t>(1, options.max_entries);
    const auto max_depth = options.recursive ? std::max<std::size_t>(1, options.max_depth) : 1;
    std::vector<std::pair<fs::path, std::size_t>> stack{{options.path, 0}};
    std::vector<std::string> collected, notices;
    std::size_t pruned = 0, skipped = 0;
    bool timed_out = false, truncated = false;
    const auto skippable = [](const std::error_code& ec) {
        return ec == std::errc::no_such_file_or_directory || ec == std::errc::permission_denied;
    };
    while (!stack.empty()) {
        if (cancel && cancel->cancelled())
            throw Error("Recursive filesystem operation cancelled by the MCP client.");
        if (Clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        const auto [path, depth] = stack.back();
        stack.pop_back();
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (skippable(ec) || status.type() == fs::file_type::not_found) {
            ++skipped;
            continue;
        }
        if (ec)
            throw std::system_error(ec, "inspect " + path_text(path));
        const bool directory = fs::is_directory(status);
        if (depth > 0 || !directory || options.recursive) {
            const char type = directory                     ? 'd'
                              : fs::is_regular_file(status) ? 'f'
                              : fs::is_symlink(status)      ? 'l'
                                                            : '?';
            collected.push_back(std::string(1, type) + '\t' + path_text(path));
            if (collected.size() >= max_entries) {
                truncated = true;
                break;
            }
        }
        if (!directory || depth >= max_depth)
            continue;
        fs::directory_iterator iterator(path, ec);
        if (skippable(ec)) {
            ++skipped;
            continue;
        }
        if (ec)
            throw std::system_error(ec, "list " + path_text(path));
        std::vector<fs::path> children;
        for (const auto& entry : iterator) {
            if (cancel && cancel->cancelled())
                throw Error("Recursive filesystem operation cancelled by the MCP client.");
            if (Clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            children.push_back(entry.path().filename());
        }
        if (timed_out)
            break;
        std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
            const auto left = lower(path_text(a)), right = lower(path_text(b));
            return left == right ? a < b : left < right;
        });
        for (auto child = children.rbegin(); child != children.rend(); ++child) {
            if (excluded.contains(lower(path_text(*child)))) {
                ++pruned;
                continue;
            }
            stack.emplace_back(path / *child, depth + 1);
        }
    }
    if (timed_out)
        notices.push_back("listing stopped after " + std::to_string(options.timeout.count()) + " ms");
    if (truncated)
        notices.push_back("listing capped at " + std::to_string(max_entries) + " entries");
    if (pruned)
        notices.push_back("pruned " + std::to_string(pruned) + " excluded directories");
    if (skipped)
        notices.push_back("skipped " + std::to_string(skipped) + " inaccessible or vanished paths");
    ProcessOutput result;
    result.stdout_text = collected.empty() ? "" : join(collected, "\n") + '\n';
    result.stderr_text = notices.empty() ? "" : join(notices, "; ") + '\n';
    return result;
}
void validate_key(std::string_view value) {
    if (value.empty() || value.size() > 80 || !std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }))
        throw Error(
            "Task/operation IDs must be 1-80 lowercase ASCII letters, digits, hyphens or underscores");
}
Json task_get(const fs::path& root, std::string_view id) {
    validate_key(id);
    const auto path = root / path_from_utf8(std::string(id) + ".json");
    DiskFile file(path);
    if (file.missing)
        return Json{{"exists", false}, {"task_id", id}, {"revision", 0}};
    if (file.size() > 65536 + 2048)
        throw Error("Task record exceeds size limit");
    const auto bytes = read_exact(file, static_cast<std::size_t>(file.size()));
    return Json{{"exists", true}, {"record", Json::parse(bytes)}, {"sha256", sha256(bytes)}};
}
Json task_put(const fs::path& root, std::string_view id, std::uint64_t revision, const Json& state) {
    validate_key(id);
    if (revision >= max_safe_integer)
        throw Error("Task revision exceeds the interoperable integer range");
    if (state.dump().size() > 65536)
        throw Error("Task state exceeds 65536 bytes; store large artifacts separately");
    fs::create_directories(root);
    FileLock gate(root / ".submission.lock");
    const auto current = task_get(root, id);
    const auto actual_revision = current["exists"] == true ? json_uint(current["record"], "revision") : 0;
    if (actual_revision == revision + 1 &&
        canonical_json(current["record"]["state"]) == canonical_json(state))
        return Json{{"replayed", true}, {"record", current["record"]}, {"sha256", current["sha256"]}};
    if (actual_revision != revision)
        throw Error("TASK_CONFLICT: expected_revision is stale");
    if (current["exists"] != true && task_ids(root).size() >= 10000)
        throw Error("TASK_CAPACITY: archive completed task records before creating more");
    const auto record = Json{{"schema_version", 1},
                             {"task_id", id},
                             {"revision", revision + 1},
                             {"updated_at", utc_now()},
                             {"state", state}};
    const auto expected = current["exists"] == true ? current["sha256"].get<std::string>() : "missing";
    const auto receipt = atomic_write(root / path_from_utf8(std::string(id) + ".json"), record.dump(), false,
                                      true, {expected, {}});
    return Json{{"replayed", receipt.replayed}, {"record", record}, {"sha256", *receipt.current.sha256}};
}
Json task_list(const fs::path& root, const std::optional<std::string>& cursor, std::size_t limit) {
    if (limit < 1 || limit > 100)
        throw Error("limit must be between 1 and 100");
    auto ids = task_ids(root);
    if (cursor)
        std::erase_if(ids, [&](const auto& id) { return id <= *cursor; });
    const auto next = ids.size() > limit ? Json(ids[limit - 1]) : Json(nullptr);
    Json records = Json::array();
    for (std::size_t i = 0; i < std::min(limit, ids.size()); ++i) {
        const auto value = task_get(root, ids[i]);
        if (value["exists"] == true)
            records.push_back(Json{{"task_id", ids[i]},
                                   {"revision", value["record"]["revision"]},
                                   {"updated_at", value["record"]["updated_at"]}});
    }
    return Json{{"tasks", records}, {"next_cursor", next}};
}
} // namespace devbox
