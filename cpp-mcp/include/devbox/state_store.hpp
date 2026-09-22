#pragma once
#include "storage.hpp"
namespace devbox {
inline constexpr unsigned state_store_schema_version = 2;
struct StateRecord {
    std::string kind, id, principal, group, status;
    std::uint64_t revision = 0;
    Json data = Json::object();
};
struct StateMutation {
    StateRecord record;
    std::uint64_t expected_revision = 0;
};
struct StateEvent {
    std::string run, type;
    std::uint64_t sequence = 0;
    Json data = Json::object();
};
struct StateQuery {
    std::string kind;
    std::optional<std::string> principal, group, status, after;
    std::size_t limit = 50;
};
struct StatePage {
    std::vector<StateRecord> records;
    std::optional<std::string> next;
};
struct StateCountQuery {
    std::string kind;
    std::optional<std::string> principal, group;
    std::vector<std::string> statuses;
    std::size_t limit = 256;
};
struct StateStoreOptions {
    bool writable = true;
    Millis writer_wait{1000};
    std::uint64_t maximum_bytes = 1024ULL * 1024 * 1024;
    // Native fault injection only. No environment variable or MCP argument exposes this hook.
    std::function<void(std::string_view)> transition_hook;
};
class StateStore {
  public:
    virtual ~StateStore() = default;
    virtual std::optional<StateRecord> get(std::string_view kind, std::string_view id) const = 0;
    virtual StatePage list(const StateQuery& query) const = 0;
    virtual std::uint64_t count(std::string_view kind, const std::optional<std::string>& principal = {},
                                const std::optional<std::string>& status = {}) const = 0;
    // Admission needs only a bounded count up to its cap, under one consistent writer observation.
    virtual std::uint64_t count_matching(const StateCountQuery& query) const = 0;
    // A single transaction: compare every revision, persist every mutation and append ordered events.
    // Operation/receipt records are never deleted; uncertain external effects retain their identity.
    virtual void apply(std::span<const StateMutation> mutations, std::span<const StateEvent> events = {}) = 0;
    // Retain a receipt for a critical coordinator request whose acknowledgement can be lost.
    // This deduplicates the state transaction, not an arbitrary external side effect.
    virtual bool apply_once(std::string_view batch_id, std::span<const StateMutation> mutations,
                            std::span<const StateEvent> events = {}) = 0;
    // Migration inserts missing identities at their original revision; it cannot overwrite history.
    virtual void import_records(std::span<const StateRecord> records) = 0;
    virtual std::vector<StateEvent> events(std::string_view run, std::uint64_t after,
                                           std::size_t limit) const = 0;
    virtual std::uint64_t generation() const = 0;
    virtual void release_writer() = 0;
    virtual Json diagnostics() const = 0;
};
std::shared_ptr<StateStore> open_state_store(const fs::path& directory, StateStoreOptions options = {});
void ensure_private_state_directory(const fs::path& directory);
} // namespace devbox
