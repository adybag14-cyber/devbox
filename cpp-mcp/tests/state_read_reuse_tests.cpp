#include "devbox/state_store.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <thread>
using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class F> void rejects(F&& callback) {
    bool rejected = false;
    try {
        callback();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "invalid request must be rejected");
}
void fixture_sql(const fs::path& root, const char* sql) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path_text(root / "metadata.sqlite3").c_str(), &db) == SQLITE_OK,
            "private fixture database opens");
    ScopeExit close([&] { sqlite3_close(db); });
    require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "private fixture SQL succeeds");
}
void check_mode(const fs::path& root, bool reuse) {
    StateStoreOptions options;
    options.reuse_read_statements = reuse;
    auto writer = open_state_store(root, options);
    std::vector<StateMutation> seed;
    for (unsigned i = 0; i < 120; ++i) {
        seed.push_back({{"job", "row-" + std::to_string(1000 + i), "owner-" + std::to_string(i % 4),
                         "group-" + std::to_string(i % 3), i % 2 ? "running" : "done", 0,
                         Json{{"i", i}, {"text", "literal ${NO_EXPANSION}\\n \xce\xa9"}}},
                        0});
    }
    std::vector<StateEvent> events;
    for (unsigned i = 0; i < 8; ++i)
        events.push_back({i % 2 ? "run-a" : "run-b", "observed", 0, Json{{"i", i}}});
    writer->apply(seed, events);
    options.writable = false;
    auto reader = open_state_store(root, options);
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned i = 0; i < 120; ++i) {
            const auto row = reader->get("job", seed[i].record.id);
            require(row && row->data == seed[i].record.data && row->principal == seed[i].record.principal,
                    "rebound get preserves exact principal and payload");
            require(!reader->get("job", "missing") && !reader->get("other-kind", seed[i].record.id),
                    "missing get does not retain the previous result");
        }
        // Every optional filter combination uses a distinct, fixed SQL slot.
        for (unsigned mask = 0; mask < 8; ++mask) {
            StateQuery query{"job"};
            query.limit = 3;
            if (mask & 1)
                query.principal = "owner-1";
            if (mask & 2)
                query.group = "group-1";
            if (mask & 4)
                query.status = "running";
            std::vector<std::string> expected, actual;
            for (const auto& row : seed) {
                if ((!query.principal || row.record.principal == *query.principal) &&
                    (!query.group || row.record.group == *query.group) &&
                    (!query.status || row.record.status == *query.status))
                    expected.push_back(row.record.id);
            }
            for (unsigned page = 0; page < 121; ++page) {
                auto result = reader->list(query);
                for (const auto& row : result.records)
                    actual.push_back(row.id);
                if (!result.next)
                    break;
                query.after = result.next;
                require(page < 120, "pagination must make bounded progress");
            }
            require(actual == expected, "all eight list shapes paginate without binding bleed");
            query.after = "zzzz";
            require(reader->list(query).records.empty(), "empty page does not retain rows");
        }
        for (unsigned mask = 0; mask < 4; ++mask) {
            StateCountQuery query{"job"};
            query.limit = 17;
            if (mask & 1)
                query.principal = "owner-1";
            if (mask & 2)
                query.group = "group-1";
            query.statuses = {"done", "running", "running", "missing"};
            unsigned expected = 0;
            for (const auto& row : seed)
                if ((!query.principal || row.record.principal == *query.principal) &&
                    (!query.group || row.record.group == *query.group))
                    ++expected;
            require(reader->count_matching(query) == std::min(17U, expected),
                    "all four count shapes preserve distinct statuses and caps");
            query.statuses = {"missing"};
            require(reader->count_matching(query) == 0, "empty count rebinds its status");
        }
        require(reader->count("job") == 120 && reader->count("job", "owner-1") == 30 &&
                    reader->count("job", "owner-1", "done") == 0 && reader->count("missing") == 0,
                "materialized counts do not retain a prior filter");
        require(reader->events("run-a", 0, 2).size() == 2 && reader->events("run-a", 2, 4).size() == 2 &&
                    reader->events("run-b", 0, 4).size() == 4 && reader->events("missing", 0, 4).empty(),
                "event queries release early reads and rebind run/cursor/limit");
        rejects([&] { (void)reader->events("run-a", std::numeric_limits<std::uint64_t>::max(), 1); });
        require(reader->events("run-a", 0, 4).size() == 4, "bind exception leaves the slot reusable");
        rejects([&] { (void)reader->list(StateQuery{"job", {}, {}, {}, {}, 101}); });
        rejects([&] { (void)reader->get("*", "row-1000"); });
        rejects([&] { (void)reader->count_matching(StateCountQuery{"job", {}, {}, {}, 10}); });
        require(reader->count("job") == 120, "validation failures do not poison later reads");
        (void)reader->count("job");
        const auto steps = json_uint(reader->diagnostics(), "last_query_vm_steps");
        (void)reader->count("job");
        require(json_uint(reader->diagnostics(), "last_query_vm_steps") == steps,
                "per-call VM steps do not accumulate across cached executions");
    }
    // Reused SELECT programs must not retain a read snapshot across calls.
    auto update = *writer->get("job", "row-1000");
    update.data["revision_marker"] = 2;
    StateMutation change{update, update.revision};
    writer->apply({&change, 1});
    require(reader->get("job", "row-1000")->data["revision_marker"] == 2,
            "readonly connection observes the newest committed WAL state");
    fixture_sql(root, "UPDATE records SET data='malformed json' WHERE kind='job' AND id='row-1001'");
    rejects([&] { (void)reader->get("job", "row-1001"); });
    fixture_sql(root, "UPDATE records SET data='{\"i\":1}' WHERE kind='job' AND id='row-1001'");
    require(reader->get("job", "row-1001")->data["i"] == 1,
            "JSON failure releases the transaction and clears bindings");
    fixture_sql(root, "CREATE INDEX fixture_schema_change ON records(status,kind)");
    require(reader->count("job") == 120, "SQLite automatic reprepare preserves cached query semantics");
    fixture_sql(root, "DROP INDEX fixture_schema_change");

    StateQuery indexed{"job", "owner-1", {}, {}, {}, 3};
    require(reader->list(indexed).records.size() == 3, "prepare the fixed indexed shape");
    fixture_sql(root, "DROP INDEX records_principal");
    rejects([&] { (void)reader->list(indexed); });
    fixture_sql(root, "CREATE INDEX records_principal ON records(kind,principal,id)");
    require(reader->list(indexed).records.size() == 3, "SQL error discards the failed compiled statement");
    std::vector<StateMutation> large;
    for (unsigned i = 0; i < 6; ++i)
        large.push_back({{"large", "large-" + std::to_string(i), "owner", "group", "ready", 0,
                          Json{{"payload", std::string(850000, 'x')}}},
                         0});
    writer->apply(large);
    StateQuery bounded{"large"};
    bounded.limit = 10;
    const auto first = reader->list(bounded);
    require(first.records.size() == 4 && first.next.has_value(),
            "four MiB page budget stops before fifth row");
    bounded.after = first.next;
    const auto rest = reader->list(bounded);
    require(rest.records.size() == 2 && !rest.next,
            "large-page early return releases its transaction and cursor");
    std::atomic<unsigned> failures = 0;
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&, thread] {
            try {
                for (unsigned i = 0; i < 150; ++i) {
                    const auto slot = (i + thread * 17) % 120;
                    const auto row = reader->get("job", seed[slot].record.id);
                    if (!row || row->principal != seed[slot].record.principal || reader->count("job") != 120)
                        ++failures;
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    require(failures == 0, "shared-store readers serialize slot ownership across threads");
    writer->release_writer();
    auto replacement = open_state_store(root, StateStoreOptions{});
    rejects([&] { writer->apply({&change, 1}); });
    require(reader->get("job", "row-1000")->revision == 2 && replacement->count("job") == 120,
            "statement reuse neither bypasses writer fencing nor changes persisted state");
}
} // namespace
int main() {
    const auto root = fs::canonical(fs::temp_directory_path()) / ("devbox-read-reuse-" + uuid());
    ensure_private_state_directory(root);
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(root, ec);
    });
    try {
        for (bool reuse : {false, true})
            check_mode(root / (reuse ? "cached" : "uncached"), reuse);
        std::cout << "Read statement reuse: both modes, 15 query shapes, exceptions, WAL freshness and "
                     "threads passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
