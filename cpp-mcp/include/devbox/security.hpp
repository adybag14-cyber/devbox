#pragma once
#include "storage.hpp"
namespace devbox {
enum class AuditDecision { Granted, Admitted, Denied, Revoked, Completed, Uncertain, CancelRequested };
// IDs are issued by the control plane. Never pass arguments, prompts, exceptions or provider payloads.
struct SecurityEvent {
    AuditDecision decision;
    std::string principal, run, operation, grant;
};
class SecurityAudit {
    fs::path path_;
    std::uint64_t maximum_;

  public:
    explicit SecurityAudit(fs::path path, std::uint64_t maximum = 64 * 1024 * 1024)
        : path_(std::move(path)), maximum_(maximum) {}
    // A failed durable append throws. Admission must stop before the effect in that case.
    void append(const SecurityEvent& event);
};
struct SecretScope {
    std::string principal, run, destination;
    bool operator==(const SecretScope&) const = default;
};
class SecretBroker {
    struct Entry {
        fs::path file;
        SecretScope scope;
        std::uint64_t expires;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;

  public:
    // Only operator configuration/grant code can bind a reference. Models cannot register secrets.
    std::string bind(fs::path private_file, SecretScope scope, std::uint64_t expires_at_ms);
    void revoke(std::string_view reference);
    void use(std::string_view reference, const SecretScope& scope,
             const std::function<void(std::string_view)>& trusted_consumer) const;
};
} // namespace devbox
