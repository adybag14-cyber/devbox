#include "devbox/security.hpp"
#include <algorithm>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#endif
namespace devbox {
namespace {
bool valid_id(std::string_view id) {
    return id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
           });
}
std::string_view decision_name(AuditDecision value) {
    switch (value) {
    case AuditDecision::Admitted:
        return "admitted";
    case AuditDecision::Denied:
        return "denied";
    case AuditDecision::Revoked:
        return "revoked";
    case AuditDecision::Completed:
        return "completed";
    case AuditDecision::Uncertain:
        return "uncertain";
    case AuditDecision::CancelRequested:
        return "cancel_requested";
    }
    throw Error("Unknown security audit decision");
}
} // namespace
void SecurityAudit::append(const SecurityEvent& event) {
    for (const auto* id : {&event.principal, &event.run, &event.operation, &event.grant})
        if (!valid_id(*id))
            throw Error("Security audit requires opaque control-plane IDs");
    if (event.principal.empty())
        throw Error("Security audit principal is required");
    const auto bytes = Json{{"schema", 1},
                            {"at", utc_now()},
                            {"decision", decision_name(event.decision)},
                            {"principal_id", event.principal},
                            {"run_id", event.run},
                            {"operation_id", event.operation},
                            {"grant_id", event.grant}}
                           .dump() +
                       '\n';
    if (bytes.size() > 4096)
        throw Error("Security audit record exceeds its budget");
    ensure_directory(path_.parent_path());
    FileLock lock(path_.parent_path() / ".security-audit.lock", Millis(5000), {}, true);
    std::error_code ec;
    const auto prior = fs::exists(path_, ec) ? fs::file_size(path_, ec) : 0;
    if (ec || prior > maximum_ || bytes.size() > maximum_ - prior)
        throw Error("Security audit spool unavailable or full; admission denied");
#ifdef _WIN32
    NativeHandle file(
        CreateFileW(path_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("Security audit spool cannot be opened safely; admission denied");
    DWORD count = 0;
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
        count != bytes.size() || !FlushFileBuffers(file.get()))
        throw Error("Security audit durable write failed; admission denied");
#else
    NativeHandle file(::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat info{};
    if (!file || ::fstat(file.get(), &info) || !S_ISREG(info.st_mode))
        throw Error("Security audit spool cannot be opened safely; admission denied");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw Error("Security audit write failed; admission denied");
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(file.get()))
        throw Error("Security audit durable flush failed; admission denied");
    NativeHandle directory(::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory || ::fsync(directory.get()))
        throw Error("Security audit directory flush failed; admission denied");
#endif
}
std::string SecretBroker::bind(fs::path file, SecretScope scope, std::uint64_t expires) {
    if (!file.is_absolute() || scope.principal.empty() || scope.run.empty() || scope.destination.empty() ||
        expires <= unix_millis())
        throw Error("A secret reference requires an absolute private file, exact scope and future expiry");
    const auto reference = "secret-" + uuid();
    std::lock_guard lock(mutex_);
    if (entries_.size() >= 256)
        throw Error("Secret reference capacity reached");
    entries_.emplace(reference, Entry{std::move(file), std::move(scope), expires});
    return reference;
}
void SecretBroker::revoke(std::string_view reference) {
    std::lock_guard lock(mutex_);
    entries_.erase(std::string(reference));
}
void SecretBroker::use(std::string_view reference, const SecretScope& scope,
                       const std::function<void(std::string_view)>& consumer) const {
    // Lock spans use so revoke is a linearizable boundary for future uses.
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(std::string(reference));
    if (found == entries_.end() || found->second.scope != scope || found->second.expires <= unix_millis())
        throw Error("Secret reference unavailable for this scope");
    std::string secret;
    try {
        secret = read_file(found->second.file, 16385);
    } catch (...) {
        throw Error("Private secret material unavailable");
    }
    ScopeExit clear([&] {
        volatile char* data = secret.data();
        for (std::size_t i = 0; i < secret.size(); ++i)
            data[i] = 0;
    });
    if (secret.empty() || secret.size() > 16384 || secret.find('\0') != std::string::npos)
        throw Error("Invalid private secret material");
    try {
        consumer(secret);
    } catch (...) {
        throw Error("Scoped secret consumer failed; private diagnostics withheld");
    }
}
} // namespace devbox
