#include "devbox/state_store.hpp"
#include <iostream>
#include <sqlite3.h>

using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
void exercise(const fs::path& root) {
    auto store = open_state_store(root);
    for (unsigned start = 0; start < 2000; start += 100) {
        std::vector<StateMutation> records;
        for (unsigned i = start; i < start + 100; ++i) {
            records.push_back({{"job", "job-pruned-" + std::to_string(10000 + i), "owner", "task",
                                "succeeded", 0, Json{{"artifacts_pruned", true}, {"receipt", "must remain"}}},
                               0});
            records.push_back({{"job_usage", "job-pruned-" + std::to_string(10000 + i), "owner", "", "absent",
                                0, Json{{"bytes", 0}, {"compacted", true}}},
                               0});
        }
        store->apply(records);
    }
    std::vector<StateMutation> candidates;
    const Json markers = Json::array({false, nullptr, 1, "true", true});
    for (std::size_t i = 0; i < markers.size(); ++i)
        candidates.push_back({{"job", "job-retained-" + std::to_string(i), "owner", "task",
                               i == 4 ? "running" : "succeeded", 0, Json{{"artifacts_pruned", markers[i]}}},
                              0});
    candidates.push_back({{"job", "job-retained-5", "owner", "task", "succeeded", 0, Json::object()}, 0});
    candidates.push_back(
        {{"job", "job-retained-other", "other-owner", "task", "succeeded", 0, Json::object()}, 0});
    store->apply(candidates);
    StateQuery query{"job", "owner", {}, {}, {}, 2, true};
    const auto first = store->list(query);
    require(first.records.size() == 2 && first.records[0].id == "job-retained-0" && first.next &&
                json_uint(store->diagnostics(), "last_query_vm_steps") < 120,
            "a maintenance page seeks past thousands of pruned rows using its index");
    auto history = query;
    history.maintenance_candidates = false;
    const auto historical = store->list(history);
    require(historical.records[0].id == "job-pruned-10000" && store->count("job", "owner") == 2006 &&
                store->get("job", "job-pruned-10000")->data["receipt"] == "must remain",
            "ordinary history, counts and receipts retain every pruned identity");
    std::vector<std::string> seen;
    do {
        const auto page = store->list(query);
        require(json_uint(store->diagnostics(), "last_query_vm_steps") < 120,
                "every maintenance cursor page has bounded indexed work");
        for (const auto& row : page.records) {
            require(row.principal == "owner", "maintenance filtering preserves principal scope");
            seen.push_back(row.id);
        }
        query.after = page.next;
    } while (query.after);
    require(seen == std::vector<std::string>({"job-retained-0", "job-retained-1", "job-retained-2",
                                              "job-retained-3", "job-retained-4", "job-retained-5"}),
            "only actual terminal boolean tombstones are omitted; active, missing and malformed markers "
            "remain visible");
    for (unsigned shape = 0; shape < 4; ++shape) {
        auto invalid = query;
        if (shape == 0)
            invalid.kind = "task";
        if (shape == 1)
            invalid.principal.reset();
        if (shape == 2)
            invalid.group = "task";
        if (shape == 3)
            invalid.status = "succeeded";
        bool rejected = false;
        try {
            (void)store->list(invalid);
        } catch (const Error& error) {
            rejected = std::string_view(error.what()) == "STATE_MAINTENANCE_QUERY_SHAPE";
        }
        require(rejected, "unsupported maintenance query shapes fail explicitly");
    }
    store.reset();
    // Model an existing schema-v2 deployment that predates this optional index.
    sqlite3* legacy = nullptr;
    require(sqlite3_open(path_text(root / "metadata.sqlite3").c_str(), &legacy) == SQLITE_OK,
            "open closed fixture database for older-layout simulation");
    ScopeExit close([&] { sqlite3_close(legacy); });
    require(sqlite3_exec(legacy,
                         "DROP TRIGGER job_maintenance_insert; DROP TRIGGER job_maintenance_update; "
                         "DROP TABLE job_maintenance_candidates;",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "remove only the fixture's additive maintenance index");
    sqlite3_close(legacy);
    close.disarm();
    StateStoreOptions readonly;
    readonly.writable = false;
    auto old_reader = open_state_store(root, readonly);
    require(old_reader->list(history).records[0].id == "job-pruned-10000",
            "ordinary read-only access remains compatible with older schema-v2 databases");
    old_reader.reset();
    auto upgraded = open_state_store(root);
    query.after.reset();
    require(upgraded->list(query).records[0].id == "job-pruned-10000",
            "older data is conservatively queued for bounded reconciliation at additive upgrade");
    for (unsigned start = 0; start < 2000; start += 100) {
        std::vector<StateMutation> charges;
        for (unsigned i = start; i < start + 100; ++i) {
            auto value = *upgraded->get("job_usage", "job-pruned-" + std::to_string(10000 + i));
            charges.push_back({value, value.revision});
        }
        upgraded->apply(charges);
    }
    require(upgraded->list(query).records[0].id == "job-retained-0" && upgraded->count("job") == 2007,
            "reconciliation settles the derived index without deleting historical records");
    require(upgraded->get("job", "job-pruned-10000")->revision == 1,
            "reconciling the charge does not rewrite the immutable job identity");
    auto changed = *upgraded->get("job_usage", "job-pruned-10000");
    changed.data["bytes"] = 1;
    const StateMutation changed_charge{changed, changed.revision};
    upgraded->apply({&changed_charge, 1});
    require(upgraded->list(query).records[0].id == changed.id,
            "a changed charge is conservatively requeued even for a pruned job");
    changed = *upgraded->get("job_usage", changed.id);
    changed.data["bytes"] = 0;
    const StateMutation corrected_charge{changed, changed.revision};
    upgraded->apply({&corrected_charge, 1});
    auto reader = open_state_store(root, readonly);
    require(reader->list(query).records[0].id == "job-retained-0",
            "the additive index is visible to read-only WAL readers");
    reader.reset();
    upgraded.reset();
    sqlite3* older_writer = nullptr;
    require(sqlite3_open(path_text(root / "metadata.sqlite3").c_str(), &older_writer) == SQLITE_OK,
            "open closed fixture for an older writer's ordinary SQL update");
    ScopeExit close_older([&] { sqlite3_close(older_writer); });
    require(sqlite3_exec(older_writer,
                         "UPDATE records SET revision=revision+1 "
                         "WHERE kind='job' AND id='job-pruned-10000';",
                         nullptr, nullptr, nullptr) == SQLITE_OK,
            "older SQL writers remain compatible with the additive triggers");
    sqlite3_close(older_writer);
    close_older.disarm();
    auto reopened = open_state_store(root);
    require(reopened->list(query).records[0].id == "job-pruned-10000",
            "an older writer conservatively requeues an otherwise settled pair");
    const auto revisited = *reopened->get("job_usage", "job-pruned-10000");
    const StateMutation recheck{revisited, revisited.revision};
    reopened->apply({&recheck, 1});
    require(reopened->list(query).records[0].id == "job-retained-0" &&
                reopened->get("job", "job-pruned-10000")->revision == 2,
            "reconciliation preserves the older writer's history while settling its derived index");
}
} // namespace
int main() {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-state-maintenance-" + uuid());
    if (!root.is_absolute() || !fs::create_directory(root)) {
        std::cerr << "Cannot create a new owned fixture root\n";
        return 1;
    }
    ScopeExit cleanup([&] {
        std::error_code error;
        fs::remove_all(root, error);
    });
    try {
        exercise(root / "state");
        std::cout << "Indexed maintenance filtering, historical identity and additive upgrade passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
