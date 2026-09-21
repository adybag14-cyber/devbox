#include "devbox/security.hpp"
#include <iostream>
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <typename F> bool denied(F action) {
    try {
        action();
        return false;
    } catch (const Error&) {
        return true;
    }
}
int main() {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-security-" + uuid());
    ensure_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        const auto path = root / "audit.jsonl";
        SecurityAudit audit(path, 600);
        SecurityEvent event{AuditDecision::Admitted, "principal-1", "run-1", "op-1", "grant-1"};
        audit.append(event);
        require(read_json(path)["decision"] == "admitted", "security evidence persisted before admission");
        bool full = false;
        for (int i = 0; i < 10 && !full; ++i)
            full = denied([&] { audit.append(event); });
        require(full && fs::file_size(path) <= 600,
                "audit byte limit denies admission without rotation or evidence loss");
        ensure_directory(root / "blocked");
        SecurityAudit blocked(root / "blocked");
        require(denied([&] { blocked.append(event); }), "audit spool failure is explicit and fail-closed");
        event.principal = "not-an-opaque-id\nsecret";
        require(denied([&] { audit.append(event); }), "audit forbids untyped payload identities");
        const auto secret_file = root / "private-key";
        const std::string canary = "SYNTHETIC-SCOPED-CREDENTIAL";
        write_file(secret_file, canary);
        SecretBroker broker;
        const SecretScope scope{"principal-1", "run-1", "https://provider.example"};
        const auto reference = broker.bind(secret_file, scope, unix_millis() + 60000);
        bool called = false;
        broker.use(reference, scope, [&](std::string_view secret) {
            require(secret == canary, "broker supplies the authorized secret");
            called = true;
        });
        require(called, "authorized consumer called");
        auto other = scope;
        other.run = "run-2";
        require(denied([&] { broker.use(reference, other, [](auto) {}); }), "cross-run reference denied");
        other = scope;
        other.destination = "https://other.example";
        require(denied([&] { broker.use(reference, other, [](auto) {}); }),
                "destination substitution denied");
        try {
            broker.use(reference, scope, [&](auto value) { throw Error(std::string(value)); });
        } catch (const Error& e) {
            require(std::string(e.what()).find(canary) == std::string::npos,
                    "secret consumer exceptions are payload-free");
        }
        broker.revoke(reference);
        require(denied([&] { broker.use(reference, scope, [](auto) {}); }), "revoked reference denied");
        require(denied([&] { broker.bind(secret_file, scope, unix_millis() - 1); }),
                "expired reference cannot be bound");
        require(read_file(path).find(canary) == std::string::npos,
                "credentials absent from durable audit evidence");
        std::cout << "Security audit and scoped secret boundaries passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
