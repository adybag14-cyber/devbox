#include "devbox/grants.hpp"
#include <future>
#include <iostream>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F&& fn, std::string_view expected) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos)
            return;
        throw Error("Unexpected error: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(expected));
}
int main() {
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-grants-" + uuid());
    ensure_directory(root);
    try {
        {
            auto store = open_state_store(root / "state");
            std::uint64_t now = unix_millis();
            GrantOptions options;
            options.now = [&] { return now; };
            GrantAuthority authority(store, root / "authority", options);
            const auto workspace = root / "workspace";
            ensure_directory(workspace);
            GrantDefinition definition;
            definition.context = {"principal", "run", "operation"};
            definition.tool = "fixture_tool";
            definition.arguments =
                Json{{"args", {"exact", "SYNTHETIC-GRANT-CREDENTIAL"}}, {"nested", Json{{"a", 1}, {"b", 2}}}};
            definition.workspace = workspace;
            definition.expires_at_ms = now + 1000;
            definition.egress_origins = {"https://api.example.org"};
            const auto id = authority.issue(definition);
            std::size_t effects = 0;
            const auto effect = [&](const GrantDefinition& admitted) {
                require(admitted.egress_origins == definition.egress_origins,
                        "egress scope is immutable broker input");
                ++effects;
                return Json{{"ok", true}};
            };
            auto wrong = definition.arguments;
            wrong["args"].push_back("; interpreter injection");
            rejects([&] { authority.perform(id, definition.context, "fixture_tool", wrong, effect); },
                    "POLICY_DENIED");
            auto other = definition.context;
            other.principal = "other";
            rejects([&] { authority.perform(id, other, "fixture_tool", definition.arguments, effect); },
                    "POLICY_DENIED");
            other = definition.context;
            other.run = "other";
            rejects([&] { authority.perform(id, other, "fixture_tool", definition.arguments, effect); },
                    "POLICY_DENIED");
            other = definition.context;
            other.operation = "other";
            rejects([&] { authority.perform(id, other, "fixture_tool", definition.arguments, effect); },
                    "POLICY_DENIED");
            rejects([&] { authority.perform(id, definition.context, "shell", definition.arguments, effect); },
                    "POLICY_DENIED");
            require(effects == 0, "all scope mismatches reject before effects");
            auto reordered = definition.arguments;
            reordered["nested"] = Json{{"b", 2}, {"a", 1}};
            require(authority.perform(id, definition.context, "fixture_tool", reordered,
                                      effect)["replayed"] == false &&
                        effects == 1,
                    "exact canonical arguments admit one effect");
            authority.revoke(id);
            require(authority.perform(id, definition.context, "fixture_tool", reordered,
                                      effect)["replayed"] == true &&
                        effects == 1,
                    "historical receipt remains readable after revoke without repeating effects");
            definition.context.operation = "revoked";
            const auto revoked = authority.issue(definition);
            authority.revoke(revoked);
            rejects(
                [&] {
                    authority.perform(revoked, definition.context, "fixture_tool", definition.arguments,
                                      effect);
                },
                "POLICY_DENIED");
            definition.context.operation = "expired";
            const auto expired = authority.issue(definition);
            now += 1001;
            rejects(
                [&] {
                    authority.perform(expired, definition.context, "fixture_tool", definition.arguments,
                                      effect);
                },
                "POLICY_DENIED");
            require(effects == 1, "expired and revoked admissions have no effects");
            definition.expires_at_ms = now + 1000;
            definition.context.operation = "uncertain";
            const auto uncertain = authority.issue(definition);
            rejects(
                [&] {
                    authority.perform(uncertain, definition.context, "fixture_tool", definition.arguments,
                                      [&](const auto&) -> Json {
                                          ++effects;
                                          throw Error("lost acknowledgement");
                                      });
                },
                "lost acknowledgement");
            rejects(
                [&] {
                    authority.perform(uncertain, definition.context, "fixture_tool", definition.arguments,
                                      effect);
                },
                "OPERATION_UNCERTAIN");
            require(effects == 2, "uncertain operation never silently retries its effect");
            definition.context.operation = "substitution";
            const auto executable = root / "program.bin";
            write_file(executable, "approved executable fixture");
            definition.executable = executable;
            definition.executable_sha256 = sha256_file(executable);
            const auto pinned = authority.issue(definition);
            write_file(executable, "substituted");
            rejects(
                [&] {
                    authority.perform(pinned, definition.context, "fixture_tool", definition.arguments,
                                      effect);
                },
                "EXECUTABLE_IDENTITY_CHANGED");
            definition.executable_sha256 = sha256_file(executable);
            const auto link = root / "alias.bin";
            fs::create_hard_link(executable, link);
            rejects([&] { authority.issue(definition); }, "EXECUTABLE_ALIAS_DENIED");
            fs::remove(link);
            definition.executable.clear();
            definition.executable_sha256.reset();
            definition.context.operation = "audit_failure";
            const auto spool_grant = authority.issue(definition);
            GrantOptions full = options;
            full.audit_max_bytes = 1;
            GrantAuthority blocked(store, root / "authority", full);
            rejects(
                [&] {
                    blocked.perform(spool_grant, definition.context, "fixture_tool", definition.arguments,
                                    effect);
                },
                "spool unavailable or full");
            require(effects == 2, "audit failure stops admission before the effect");
            require(authority.inspect(id).dump().find("SYNTHETIC-GRANT-CREDENTIAL") == std::string::npos &&
                        read_file(root / "authority" / "security-audit.jsonl")
                                .find("SYNTHETIC-GRANT-CREDENTIAL") == std::string::npos,
                    "security audit and exported grant view omit argument payloads and hashes");
            store->release_writer();
            std::cout << "Exact grant scopes, expiry, revocation, executable substitution, durable operation "
                         "identity and audit admission passed\n";
        }
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
