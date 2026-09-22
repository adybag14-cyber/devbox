#include "devbox/state_store.hpp"
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sqlite3.h>
#include <sstream>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F action, std::string_view expected) {
    try {
        action();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos)
            return;
        throw;
    }
    throw Error("Expected state-store rejection: " + std::string(expected));
}
std::string id(std::size_t value) {
    std::ostringstream text;
    text << "job-" << std::setw(6) << std::setfill('0') << value;
    return text.str();
}
int run(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--crash-state") {
        StateStoreOptions options;
        const std::string stage = argv[3];
        options.transition_hook = [stage](std::string_view at) {
            if (at == stage)
                std::_Exit(73);
        };
        auto store = open_state_store(path_from_utf8(argv[2]), options);
        StateMutation record{
            {"operation", "effect-1", "owner", "run-1", "admitted", 0, Json{{"effect", "not yet confirmed"}}},
            0};
        StateEvent event{"run-1", "operation_admitted", 0, Json{{"operation", "effect-1"}}};
        store->apply({&record, 1}, {&event, 1});
        return 1;
    }
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-state-" + uuid());
    ensure_directory(root);
    ScopeExit cleanup([&] {
        std::error_code error;
        fs::remove_all(root, error);
    });
    try {
        auto store = open_state_store(root / "state");
        const auto generation = store->generation();
        require(generation > 0, "durable writer generation exists");
        StateStoreOptions contender;
        contender.writer_wait = Millis(30);
        bool blocked = false;
        try {
            (void)open_state_store(root / "state", contender);
        } catch (const std::exception&) {
            blocked = true;
        }
        require(blocked, "second authoritative writer cannot enter");
        StateMutation initial{{"run", "run-1", "owner", "task-1", "created", 0, Json{{"checkpoint", 1}}}, 0};
        StateEvent created{"run-1", "created", 0, Json{{"checkpoint", 1}}};
        store->apply({&initial, 1}, {&created, 1});
        StateStoreOptions readonly;
        readonly.writable = false;
        auto reader = open_state_store(root / "state", readonly);
        require(reader->get("run", "run-1")->revision == 1 && reader->events("run-1", 0, 10).size() == 1,
                "read-only WAL readers observe committed record and event together");
        std::array<StateMutation, 2> conflict{initial, initial};
        conflict[0].expected_revision = 1;
        conflict[0].record.status = "running";
        conflict[1].record.id = "run-2";
        conflict[1].expected_revision = 3;
        rejects([&] { store->apply(conflict); }, "STATE_REVISION_CONFLICT");
        require(reader->get("run", "run-1")->status == "created" && !reader->get("run", "run-2") &&
                    store->count("run") == 1,
                "failed batches roll back records and materialized counters");
        rejects([&] { reader->apply({&initial, 1}); }, "STATE_WRITER_FENCED");
        store->release_writer();
        auto next = open_state_store(root / "state");
        require(next->generation() > generation, "a new writer receives a strictly newer fence");
        rejects([&] { store->apply({&initial, 1}); }, "STATE_WRITER_FENCED");
        store = next;
        next.reset();
        const auto started = Clock::now();
        for (std::size_t start = 0; start < 100000;) {
            std::vector<StateMutation> batch;
            while (batch.size() < 256 && start < 100000) {
                batch.push_back(
                    StateMutation{StateRecord{"job", id(start), "owner-" + std::to_string(start % 10),
                                              "task-" + std::to_string(start % 100),
                                              start % 3 ? "running" : "done", 0, Json{{"number", start}}},
                                  0});
                ++start;
            }
            store->apply(batch);
            if (start >= 10000 && start - batch.size() < 10000) {
                StateQuery query{"job"};
                query.limit = 50;
                require(store->list(query).records.size() == 50 &&
                            json_uint(store->diagnostics(), "last_query_vm_steps") < 5000,
                        "10k record pages have bounded indexed work");
            }
        }
        require(store->count("job") == 100000 && json_uint(store->diagnostics(), "last_query_vm_steps") < 100,
                "100k receipt admission uses indexed counters rather than a table scan");
        StateQuery query{"job"};
        query.principal = "owner-3";
        query.group = "task-13";
        query.status = "done";
        query.limit = 50;
        const auto page = store->list(query);
        std::cout << Json{{"filtered_rows", page.records.size()},
                          {"has_next", page.next.has_value()},
                          {"query_diagnostics", store->diagnostics()}}
                         .dump()
                  << std::endl;
        require(page.records.size() == 50 && page.next &&
                    json_uint(store->diagnostics(), "last_query_vm_steps") < 5000,
                "100k filtered list pages have bounded indexed work");
        query.after = page.next;
        const auto second = store->list(query);
        require(!second.records.empty() && second.records.front().id > page.records.back().id,
                "cursor pages cannot duplicate the preceding page");
        std::cout << Json{{"records", 100000},
                          {"last_page_vm_steps", store->diagnostics()["last_query_vm_steps"]},
                          {"seed_and_query_ms",
                           std::chrono::duration_cast<Millis>(Clock::now() - started).count()}}
                         .dump()
                  << '\n';
        store.reset();
        reader.reset();
        for (const auto* stage : {"mutation_written", "event_written", "before_commit", "after_commit"}) {
            const auto directory = root / stage;
            ProcessOptions options;
            options.timeout = Millis(10000);
            options.max_capture_chars = 8192;
            bool crashed = false;
            try {
                spawn_process(path_text(executable_path()), {"--crash-state", path_text(directory), stage},
                              options);
            } catch (const ProcessError& error) {
                crashed = error.exit_code == 73;
            }
            require(crashed, "fault fixture exited at the requested durable boundary");
            auto recovered = open_state_store(directory);
            const auto record = recovered->get("operation", "effect-1");
            const bool committed = std::string_view(stage) == "after_commit";
            require(record.has_value() == committed &&
                        recovered->events("run-1", 0, 10).size() == (committed ? 1 : 0),
                    "crash recovery exposes the whole transaction or none of it");
            if (record) {
                require(record->status == "admitted",
                        "unconfirmed external outcome is never labelled complete");
                StateMutation retry{*record, 0};
                rejects([&] { recovered->apply({&retry, 1}); }, "STATE_REVISION_CONFLICT");
            }
        }
        const auto future = root / "future";
        {
            auto prepared = open_state_store(future);
        }
        sqlite3* foreign = nullptr;
        require(sqlite3_open(path_text(future / "metadata.sqlite3").c_str(), &foreign) == SQLITE_OK,
                "schema fixture opened");
        require(sqlite3_exec(foreign, "PRAGMA user_version=999", nullptr, nullptr, nullptr) == SQLITE_OK,
                "future schema fixture written");
        sqlite3_close(foreign);
        rejects([&] { (void)open_state_store(future); }, "STATE_SCHEMA_INCOMPATIBLE");
        std::cout
            << "Indexed state, single-writer fencing, transaction crashes and downgrade refusal passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_args) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.push_back(narrow(wide_args[i]));
    std::vector<char*> values;
    for (auto& value : args)
        values.push_back(value.data());
    return run(argc, values.data());
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
