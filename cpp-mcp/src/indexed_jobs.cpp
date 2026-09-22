#include "devbox/jobs.hpp"
#include <algorithm>
namespace devbox {
namespace {
fs::path root_for(const Config& config) {
    return config.state_root.empty() ? config.project_root / "run" / "state" : config.state_root;
}
Json layout(const Config& config) {
    return Json{
        {"version", 1},
        {"backend", "sqlite"},
        {"minimum_writer_schema", 2},
        {"jobs_root", path_text(fs::absolute(config.jobs_root).lexically_normal())},
        {"tasks_root", path_text(fs::absolute(config.project_root / "run" / "tasks").lexically_normal())}};
}
bool has_legacy_state(const Config& config) {
    for (const auto& root : {config.jobs_root, config.project_root / "run" / "tasks"}) {
        if (!fs::exists(root))
            continue;
        std::size_t visited = 0;
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = path_text(entry.path().filename());
            if (++visited > 1024 || name.starts_with("job-") || entry.path().extension() == ".json")
                return true;
        }
    }
    const auto receipts = config.jobs_root / ".operations";
    return fs::exists(receipts) && fs::directory_iterator(receipts) != fs::directory_iterator();
}
Json task_reply(const StateRecord& value, bool replay = false, bool put = false) {
    const auto hash = sha256(value.data.dump());
    return put ? Json{{"replayed", replay}, {"record", value.data}, {"sha256", hash}}
               : Json{{"exists", true}, {"record", value.data}, {"sha256", hash}};
}
StateRecord job_record(const std::string& id, const Json& request, const Json& status) {
    const auto agent = request.value("agent", Json::object());
    return StateRecord{"job",
                       id,
                       "operator",
                       json_string(agent, "taskId"),
                       json_string(status, "status"),
                       0,
                       Json{{"status", status},
                            {"agent", agent},
                            {"request_sha256", sha256(canonical_json(request).dump())}}};
}
} // namespace
std::shared_ptr<StateStore> JobStore::index() const {
    if (!indexed())
        return {};
    std::lock_guard guard(*index_mutex_);
    if (index_)
        return index_;
    const auto root = root_for(*config_);
    ensure_directory(root.parent_path());
    ensure_private_state_directory(root);
    FileLock gate(root / ".layout.lock", Millis(5000), {}, true);
    const auto marker = read_json_optional(root / "layout.json");
    const auto expected = layout(*config_);
    if (marker && *marker != expected)
        throw Error("STATE_LAYOUT_MISMATCH: unsafe source or schema downgrade refused");
    if (!marker && has_legacy_state(*config_))
        throw Error("STATE_MIGRATION_REQUIRED: drain the frontend and migrate existing job/task state first");
    auto opened = open_coordinated_state(root);
    if (!marker)
        write_json_atomic(root / "layout.json", expected);
    index_ = std::move(opened);
    return index_;
}
void JobStore::require_writable_backend() const {
    if (!indexed() && fs::exists(root_for(*config_) / "layout.json"))
        throw Error("STATE_LEGACY_BACKEND_FENCED: this layout has migrated to SQLite; legacy writers cannot "
                    "be re-enabled");
}
std::optional<Json> JobStore::operation_receipt(std::string_view id) const {
    if (const auto state = index()) {
        const auto value = state->get("job_operation", id);
        return value ? std::optional(value->data) : std::nullopt;
    }
    return read_json_optional(config_->jobs_root / ".operations" / path_from_utf8(std::string(id) + ".json"),
                              65536);
}
void JobStore::write_operation_receipt(std::string_view id, const Json& receipt) const {
    if (const auto state = index()) {
        const auto previous = state->get("job_operation", id);
        if (previous && json_string(previous->data, "fingerprint") != json_string(receipt, "fingerprint"))
            throw Error("OPERATION_CONFLICT");
        StateMutation value{StateRecord{"job_operation", std::string(id), "operator",
                                        json_string(receipt.at("agent"), "taskId"),
                                        json_bool(receipt, "submitted") ? "submitted" : "admitted", 0,
                                        receipt},
                            previous ? previous->revision : 0};
        state->apply({&value, 1});
        // Compatibility/export mirror. SQLite remains authoritative after cutover.
        atomic_write(config_->jobs_root / ".operations" / path_from_utf8(std::string(id) + ".json"),
                     receipt.dump());
    } else
        atomic_write(config_->jobs_root / ".operations" / path_from_utf8(std::string(id) + ".json"),
                     receipt.dump());
}
std::uint64_t JobStore::operation_count() const {
    if (const auto state = index())
        return state->count("job_operation");
    const auto path = config_->jobs_root / ".operations";
    std::uint64_t count = 0;
    if (fs::exists(path))
        for (const auto& entry : fs::directory_iterator(path)) {
            (void)entry;
            if (++count >= config_->job_max_operations)
                break;
        }
    return count;
}
Json indexed_task_get(const JobStore& jobs, std::string_view id) {
    validate_key(id);
    const auto value = jobs.index()->get("task", id);
    return value ? task_reply(*value) : Json{{"exists", false}, {"task_id", id}, {"revision", 0}};
}
Json indexed_task_put(const JobStore& jobs, std::string_view id, std::uint64_t revision, const Json& state) {
    validate_key(id);
    if (revision >= max_safe_integer)
        throw Error("Task revision exceeds the interoperable integer range");
    if (state.dump().size() > 65536)
        throw Error("Task state exceeds 65536 bytes; store large artifacts separately");
    const auto root = jobs.config().project_root / "run" / "tasks";
    ensure_directory(root);
    FileLock gate(root / ".submission.lock");
    const auto index = jobs.index();
    const auto previous = index->get("task", id);
    const auto actual = previous ? previous->revision : 0;
    if (previous && actual == revision + 1 &&
        canonical_json(previous->data.at("state")) == canonical_json(state))
        return task_reply(*previous, true, true);
    if (actual != revision)
        throw Error("TASK_CONFLICT: expected_revision is stale");
    if (!previous && index->count("task") >= 10000)
        throw Error("TASK_CAPACITY: archive completed task records before creating more");
    const auto record = Json{{"schema_version", 1},
                             {"task_id", id},
                             {"revision", revision + 1},
                             {"updated_at", utc_now()},
                             {"state", state}};
    StateMutation mutation{StateRecord{"task", std::string(id), "operator", "", "checkpoint", 0, record},
                           revision};
    index->apply({&mutation, 1});
    atomic_write(root / path_from_utf8(std::string(id) + ".json"), record.dump());
    return task_reply(mutation.record, false, true);
}
Json indexed_task_list(const JobStore& jobs, const std::optional<std::string>& cursor, std::size_t limit) {
    if (limit < 1 || limit > 100)
        throw Error("limit must be between 1 and 100");
    StateQuery query{"task"};
    query.after = cursor;
    query.limit = limit;
    const auto page = jobs.index()->list(query);
    Json records = Json::array();
    for (const auto& value : page.records)
        records.push_back(Json{{"task_id", value.id},
                               {"revision", value.revision},
                               {"updated_at", value.data.at("updated_at")}});
    return Json{{"tasks", records}, {"next_cursor", page.next ? Json(*page.next) : Json()}};
}
Json migrate_legacy_state(std::shared_ptr<const Config> config) {
    const auto root = root_for(*config);
    ensure_directory(config->project_root / "run");
    FileLock frontend_gate(config->project_root / "run" / ".frontend.lock", Millis(50), {}, true);
    if (const auto pid_file = config->project_root / "run" / "mcp.pid"; fs::exists(pid_file)) {
        const auto value = trim(read_file(pid_file, 128));
        try {
            const auto pid = std::stoull(value);
            if (pid && pid <= UINT32_MAX && process_alive(static_cast<std::uint32_t>(pid)))
                throw Error(
                    "STATE_MIGRATION_FRONTEND_ACTIVE: drain and stop the recorded frontend before migration");
        } catch (const std::invalid_argument&) {
            throw Error("STATE_MIGRATION_INVALID_FRONTEND_RECORD");
        }
    }
    ensure_directory(config->jobs_root);
    const auto tasks = config->project_root / "run" / "tasks";
    ensure_directory(tasks);
    FileLock job_gate(config->jobs_root / ".submission.lock");
    FileLock task_gate(tasks / ".submission.lock");
    ensure_directory(root.parent_path());
    ensure_private_state_directory(root);
    FileLock gate(root / ".layout.lock", Millis(5000), {}, true);
    if (const auto marker = read_json_optional(root / "layout.json")) {
        if (*marker != layout(*config))
            throw Error("STATE_LAYOUT_MISMATCH");
        return Json{{"migrated", true}, {"replayed", true}};
    }
    // Migration requires the operator to drain/stop the serving frontend. Existing runners must
    // finish before changing their metadata authority; no process is terminated by migration.
    auto legacy_config = std::make_shared<Config>(*config);
    legacy_config->state_backend = "legacy";
    JobStore legacy(legacy_config);
    const auto ids = job_ids(config->jobs_root, 100000);
    for (const auto& id : ids)
        if (!terminal_status(json_string(legacy.get_status(id), "status")))
            throw Error("STATE_MIGRATION_ACTIVE_JOB: wait for durable runners to reach a terminal state");
    const auto index = open_coordinated_state(root);
    std::uint64_t jobs = 0, operations = 0, checkpoints = 0;
    std::vector<StateRecord> pending;
    std::size_t pending_bytes = 0;
    auto flush = [&] {
        if (!pending.empty()) {
            index->import_records(pending);
            pending.clear();
            pending_bytes = 0;
        }
    };
    auto import = [&](StateRecord record) {
        if (!record.revision)
            record.revision = 1;
        const auto bytes = record.data.dump().size() + 2048;
        if (pending.size() >= 128 || (!pending.empty() && pending_bytes + bytes > 2 * 1024 * 1024))
            flush();
        pending_bytes += bytes;
        pending.push_back(std::move(record));
    };
    for (const auto& id : ids) {
        import(job_record(id, legacy.read_request(id), legacy.read_status_raw(id)));
        ++jobs;
    }
    const auto receipts = config->jobs_root / ".operations";
    if (fs::exists(receipts))
        for (const auto& file : fs::directory_iterator(receipts)) {
            if (!file.is_regular_file() || file.is_symlink() || file.path().extension() != ".json")
                continue;
            const auto value = read_json(file.path(), 65536);
            const auto id = validate_job_id(json_string(value, "id"));
            if (path_text(file.path().stem()) != id)
                throw Error("STATE_MIGRATION_RECEIPT_ID_MISMATCH");
            import(StateRecord{"job_operation", id, "operator", json_string(value.at("agent"), "taskId"),
                               json_bool(value, "submitted") ? "submitted" : "admitted", 0, value});
            ++operations;
        }
    for (const auto& file : fs::directory_iterator(tasks)) {
        if (!file.is_regular_file() || file.is_symlink() || file.path().extension() != ".json")
            continue;
        const auto id = path_text(file.path().stem());
        validate_key(id);
        const auto value = task_get(tasks, id);
        const auto record = value.at("record");
        import(StateRecord{"task", id, "operator", "", "checkpoint", json_uint(record, "revision"), record});
        ++checkpoints;
    }
    flush();
    write_json_atomic(root / "layout.json", layout(*config));
    return Json{{"migrated", true},         {"replayed", false},    {"jobs", jobs},
                {"operations", operations}, {"tasks", checkpoints}, {"generation", index->generation()}};
}
} // namespace devbox
