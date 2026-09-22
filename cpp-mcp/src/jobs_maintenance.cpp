#include "devbox/filesystem_worker.hpp"
#include "jobs_internal.hpp"
#include <algorithm>
#include <fstream>
#include <set>
namespace devbox {
namespace {
Json empty_summary() {
    return Json{{"discovered", 0},
                {"scanned", 0},
                {"batchLimited", false},
                {"interrupted", 0},
                {"active", 0},
                {"terminal", 0},
                {"maintained", 0},
                {"compactedLogs", 0},
                {"deleted", 0},
                {"errors", 0},
                {"cycleCompleted", false},
                {"maintenanceCycle", 0},
                {"maintenanceCursor", 0},
                {"maintenanceTotal", 0},
                {"maintenanceProgressPercent", 0},
                {"maintenanceCycleAgeMs", 0},
                {"storeBytes", 0},
                {"terminalRetained", 0},
                {"quotaDeleted", 0},
                {"quotaPressure", false},
                {"quotaScanned", 0},
                {"quotaFullReconcile", false}};
}
void increment(Json& object, std::string_view key) {
    object[std::string(key)] = json_uint(object, key) + 1;
}
std::vector<std::string> discover(const fs::path& root) {
    std::vector<std::string> ids;
    const auto deadline = Clock::now() + Millis(5000);
    for (const auto& entry : fs::directory_iterator(root)) {
        if (Clock::now() > deadline || ids.size() >= 100000)
            throw Error("Job maintenance index exceeds its scan budget");
        if (!entry.is_directory() || entry.is_symlink())
            continue;
        const auto name = path_text(entry.path().filename());
        try {
            validate_job_id(name);
            ids.push_back(name);
        } catch (const Error&) {
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}
void ensure_index(auto& state, const fs::path& root) {
    bool refresh;
    {
        std::lock_guard lock(state.mutex);
        refresh = !state.refreshed || Clock::now() - *state.refreshed >= std::chrono::hours(1);
    }
    if (!refresh)
        return;
    auto ids = discover(root);
    std::lock_guard lock(state.mutex);
    // Merge creations that raced discovery; remove vanished paths on normal reconciliation.
    for (const auto& id : state.ids)
        if (std::find(ids.begin(), ids.end(), id) == ids.end() && fs::exists(root / path_from_utf8(id)))
            ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    state.ids = std::move(ids);
    state.cursor = 0;
    state.cycle_started.reset();
    state.refreshed = Clock::now();
}
void note_deleted(auto& state, const std::set<std::string>& deleted) {
    std::lock_guard lock(state.mutex);
    const auto end = std::min(state.cursor, state.ids.size());
    std::size_t before = 0;
    for (std::size_t i = 0; i < end; ++i)
        if (deleted.contains(state.ids[i]))
            ++before;
    std::erase_if(state.ids, [&](const auto& id) { return deleted.contains(id); });
    state.cursor = std::min(state.cursor - std::min(before, state.cursor), state.ids.size());
    for (const auto& id : deleted)
        state.quota_entries.erase(id);
}
std::uint64_t directory_bytes(const fs::path& path) {
    std::uint64_t bytes = 0;
    std::size_t entries = 0;
    std::vector<fs::path> pending{path};
    while (!pending.empty()) {
        const auto directory = pending.back();
        pending.pop_back();
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (++entries > 1024)
                throw Error("JOB_DIRECTORY_ENTRY_BUDGET");
            const auto status = entry.symlink_status();
            if (fs::is_symlink(status))
                throw Error("JOB_DIRECTORY_ALIAS_DENIED");
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw Error("JOB_DIRECTORY_ALIAS_DENIED");
#endif
            const auto relative = entry.path().lexically_relative(path);
            if (fs::is_directory(status)) {
                if (relative != fs::path("research") && relative != fs::path("research") / "documents")
                    throw Error("JOB_DIRECTORY_UNEXPECTED_ENTRY");
                pending.push_back(entry.path());
            } else if (fs::is_regular_file(status)) {
                if (relative.has_parent_path() && relative != fs::path("research") / "ledger.json") {
                    const auto name = path_text(relative.filename());
                    const auto stem = name.size() > 6 ? name.substr(1, name.size() - 6) : "";
                    if (relative.parent_path() != fs::path("research") / "documents" || name.front() != 's' ||
                        !name.ends_with(".json") || stem.empty() || stem.size() > 3 ||
                        !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
                        throw Error("JOB_DIRECTORY_UNEXPECTED_ENTRY");
                }
                const auto size = entry.file_size();
                if (size > UINT64_MAX - bytes)
                    throw Error("JOB_DIRECTORY_SIZE_OVERFLOW");
                bytes += size;
            } else
                throw Error("JOB_DIRECTORY_UNEXPECTED_ENTRY");
        }
    }
    return bytes;
}
std::int64_t completed_time(const Json& status) {
    auto parsed = parse_utc(json_string(status, "completedAtUtc"));
    if (!parsed)
        parsed = parse_utc(json_string(status, "createdAtUtc"));
    return parsed.value_or(0);
}
bool remove_job_directory(const fs::path& root, const JobPaths& paths, bool prior_prune_intent = false) {
    // Deletion authority is a validated direct child of this store, never an alias.
    if (paths.dir.lexically_normal().parent_path() != root.lexically_normal())
        throw Error("Job directory escaped configured root");
    std::error_code ec;
    const auto status = fs::symlink_status(paths.dir, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return true;
    if (ec)
        throw std::system_error(ec);
    if (!fs::is_directory(status) || fs::is_symlink(status))
        throw Error("Job cleanup rejects directory aliases");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(paths.dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error("Job cleanup rejects reparse points");
#endif
    (void)directory_bytes(paths.dir);
    const auto current = read_json_optional(paths.status);
    if ((!current && !prior_prune_intent) || (current && !terminal_status(json_string(*current, "status"))))
        return false;
    fs::remove_all(paths.dir);
    return true;
}
bool compact_log(const fs::path& path, std::uint64_t maximum) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return false;
    if (ec)
        throw std::system_error(ec);
    const auto limit = std::max<std::uint64_t>(4096, maximum);
    if (size <= limit)
        return false;
    if (limit > static_cast<std::uint64_t>(SIZE_MAX))
        throw Error("Log compaction limit exceeds addressable size");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw Error("Cannot open legacy log for compaction");
    input.seekg(static_cast<std::streamoff>(size - limit));
    std::string bytes(static_cast<std::size_t>(limit), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    input.close();
    atomic_write(path, bytes, false, false);
    return true;
}
} // namespace
Json job_filesystem_operation(const Json& args) {
    const auto root = path_from_utf8(json_string(args, "root"));
    const auto id = validate_job_id(json_string(args, "id"));
    const auto path = root / path_from_utf8(id);
    std::error_code error;
    const auto attributes = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(attributes)))
        return Json{{"exists", false}, {"bytes", 0}, {"deleted", json_bool(args, "prior_prune_intent")}};
    if (error)
        throw std::system_error(error);
    if (!root.is_absolute() || !fs::is_directory(attributes) || fs::is_symlink(attributes))
        throw Error("JOB_DIRECTORY_ALIAS_DENIED");
#ifdef _WIN32
    const auto native = GetFileAttributesW(path.c_str());
    if (native == INVALID_FILE_ATTRIBUTES || native & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error("JOB_DIRECTORY_ALIAS_DENIED");
#endif
    // Only this broker's bounded artifact layout can be reclaimed, including its research ledger/documents.
    const auto before = directory_bytes(path);
    const auto status = read_json_optional(path / "status.json", 1024 * 1024);
    const bool prior_intent = json_bool(args, "prior_prune_intent");
    const bool terminal = status ? terminal_status(json_string(*status, "status")) : prior_intent;
    const bool reclaimable =
        terminal && (!status || (json_string(*status, "status") != "interrupted" &&
                                 json_bool(*status, "workloadTerminationVerified", true) &&
                                 !json_bool(*status, "terminationPending")));
    if (json_bool(args, "remove") && reclaimable) {
        JobPaths paths;
        paths.id = id;
        paths.dir = path;
        paths.status = path / "status.json";
        const bool removed = remove_job_directory(root, paths, prior_intent);
        return Json{{"exists", !removed}, {"deleted", removed}, {"bytes", removed ? 0 : before}};
    }
    bool compacted = false;
    if (terminal && json_bool(args, "compact")) {
        const auto limit = std::clamp<std::uint64_t>(json_uint(args, "log_limit", 33554432), 4096, 67108864);
        const bool out = compact_log(path / "stdout.log", limit);
        const bool err = compact_log(path / "stderr.log", limit);
        compacted = out || err;
    }
    return Json{{"exists", true},
                {"deleted", false},
                {"bytes", compacted ? directory_bytes(path) : before},
                {"compacted", compacted}};
}

Json JobStore::indexed_maintenance(std::size_t maximum, bool quota, const Cancel& cancel) {
    auto store = index();
    const auto state_root =
        config_->state_root.empty() ? config_->project_root / "run" / "state" : config_->state_root;
    FileLock gate(state_root / ".maintenance.lock", Millis(5000), cancel, true);
    auto meter =
        store->get("maintenance", "job-usage")
            .value_or(StateRecord{"maintenance", "job-usage", "operator", "", "partial", 0, Json::object()});
    const auto cursor = json_string(meter.data, "after");
    const auto page = store->list(StateQuery{"job",
                                             "operator",
                                             {},
                                             {},
                                             cursor.empty() ? std::nullopt : std::optional(cursor),
                                             std::clamp<std::size_t>(maximum, 1, 64)});
    auto bytes = json_uint(meter.data, "bytes"), retained = json_uint(meter.data, "terminal_retained");
    auto summary = empty_summary();
    std::vector<StateMutation> mutations;
    const auto pressure = [&] {
        return (config_->job_store_max_bytes && bytes > config_->job_store_max_bytes) ||
               (config_->job_store_max_terminal_jobs && retained > config_->job_store_max_terminal_jobs);
    };
    const bool was_pressure = pressure();
    mutations.reserve(page.records.size() * 2 + 1);
    for (auto record : page.records) {
        const auto before_bytes = bytes, before_retained = retained;
        const auto before_mutations = mutations.size();
        if (cancel)
            cancel->check();
        try {
            if (!terminal_status(record.status) || record.status == "cancelled") {
                (void)get_status(record.id);
                record = *store->get("job", record.id);
            }
            const auto status = record.data.at("status");
            const bool terminal = terminal_status(record.status);
            increment(summary, terminal ? "terminal" : "active");
            auto usage = store->get("job_usage", record.id)
                             .value_or(StateRecord{"job_usage", record.id, "operator", "", "unknown", 0,
                                                   Json::object()});
            const auto old_bytes = json_uint(usage.data, "bytes");
            const auto completed = completed_time(status);
            const auto now = static_cast<std::int64_t>(unix_millis());
            const bool expired =
                config_->job_retention_hours && completed > 0 && now >= completed &&
                static_cast<std::uint64_t>(now - completed) / 3600000 >= config_->job_retention_hours;
            const bool producer_alive =
                owner_process_alive(Json{{"pid", status.value("runnerPid", Json())},
                                         {"processInstance", status.value("runnerProcessInstance", Json())}});
            const bool remove = terminal && !producer_alive && record.status != "interrupted" &&
                                json_bool(status, "workloadTerminationVerified", true) &&
                                !json_bool(status, "terminationPending") &&
                                (expired || (quota && pressure()) ||
                                 json_string(record.data, "artifacts_prune_state") == "pending");
            if (remove && !json_bool(record.data, "artifacts_pruned") &&
                json_string(record.data, "artifacts_prune_state") != "pending") {
                record.data["artifacts_prune_state"] = "pending";
                const StateMutation intent{record, record.revision};
                (void)store->apply_once("prune-" + record.id + "-" + std::to_string(record.revision),
                                        {&intent, 1});
                ++record.revision;
            }
            Json observation{{"exists", false}, {"bytes", 0}, {"deleted", false}};
            if (!json_bool(record.data, "artifacts_pruned"))
                observation =
                    isolated_filesystem("job_files",
                                        Json{{"root", path_text(config_->jobs_root)},
                                             {"id", record.id},
                                             {"remove", remove},
                                             {"prior_prune_intent",
                                              json_string(record.data, "artifacts_prune_state") == "pending"},
                                             {"compact", terminal && !json_bool(usage.data, "compacted")},
                                             {"log_limit", config_->job_log_max_bytes}},
                                        Millis(5000), cancel);
            const auto observed = json_uint(observation, "bytes");
            if (old_bytes > bytes || observed > UINT64_MAX - (bytes - old_bytes))
                throw Error("JOB_USAGE_ACCOUNTING_INTEGRITY");
            bytes = bytes - old_bytes + observed;
            if (usage.status == "retained_terminal") {
                if (!retained)
                    throw Error("JOB_USAGE_ACCOUNTING_INTEGRITY");
                --retained;
            }
            usage.status =
                json_bool(observation, "exists") ? (terminal ? "retained_terminal" : "active") : "absent";
            if (usage.status == "retained_terminal")
                ++retained;
            usage.data = Json{{"bytes", observed}, {"checked_at", utc_now()}, {"compacted", terminal}};
            mutations.push_back({usage, usage.revision});
            if (json_bool(observation, "compacted"))
                increment(summary, "compactedLogs");
            if (json_bool(observation, "deleted")) {
                record.data["artifacts_pruned"] = true;
                record.data["artifacts_prune_state"] = "completed";
                record.data["artifacts_pruned_at"] = utc_now();
                mutations.push_back({record, record.revision});
                increment(summary, "deleted");
                if (quota && !expired)
                    increment(summary, "quotaDeleted");
            }
            increment(summary, "maintained");
        } catch (const Cancelled&) {
            throw;
        } catch (const Error& error) {
            if (std::string_view(error.what()).starts_with("JOB_USAGE_ACCOUNTING_"))
                throw;
            bytes = before_bytes;
            retained = before_retained;
            mutations.resize(before_mutations);
            increment(summary, "errors");
        }
    }
    // One transaction commits both each sampled charge and its aggregate/cursor. Lost acknowledgements
    // cannot count a charge twice; a new pass reloads the committed revisions before doing more work.
    meter.data["bytes"] = bytes;
    meter.data["terminal_retained"] = retained;
    meter.data["after"] = page.next.value_or("");
    meter.data["checked_at"] = utc_now();
    meter.data["cycles"] = json_uint(meter.data, "cycles") + (page.next ? 0 : 1);
    meter.data["cycle_errors"] =
        (cursor.empty() ? 0 : json_uint(meter.data, "cycle_errors")) + json_uint(summary, "errors");
    meter.status = page.next || json_uint(meter.data, "cycle_errors") ? "partial" : "sampled";
    mutations.push_back({meter, meter.revision});
    store->apply(mutations);
    {
        std::lock_guard lock(maintenance_->mutex);
        maintenance_->store_bytes = bytes;
        maintenance_->terminal_retained = retained;
        maintenance_->quota_pressure = pressure();
        maintenance_->quota_cycle_pending =
            page.next.has_value() || (pressure() && json_uint(summary, "deleted") > 0);
        maintenance_->quota_checked = Clock::now();
        maintenance_->quota_checked_utc = json_string(meter.data, "checked_at");
    }
    summary["scanned"] = page.records.size();
    summary["quotaScanned"] = page.records.size();
    summary["batchLimited"] = page.next.has_value();
    summary["cycleCompleted"] = !page.next;
    summary["maintenanceCycle"] = meter.data["cycles"];
    summary["accounting"] = "persistent_incremental_samples";
    summary["accountingComplete"] = meter.status == "sampled";
    summary["pressureAtEntry"] = was_pressure;
    summary["reclaimOrder"] = "eligible_terminal_jobs_in_index_page_order";
    summary.update(quota_snapshot());
    return summary;
}
Json JobStore::quota_snapshot() const {
    std::lock_guard lock(maintenance_->mutex);
    Json value{{"storeBytes", maintenance_->store_bytes},
               {"terminalRetained", maintenance_->terminal_retained},
               {"quotaPressure", maintenance_->quota_pressure},
               {"quotaCyclePending", maintenance_->quota_cycle_pending}};
    if (maintenance_->quota_checked) {
        value["quotaCheckedAtUtc"] = maintenance_->quota_checked_utc;
        value["quotaAgeMs"] =
            std::chrono::duration_cast<Millis>(Clock::now() - *maintenance_->quota_checked).count();
    }
    return value;
}
Json JobStore::reconcile_maintenance(std::size_t maximum, const Cancel& cancel) {
    if (indexed())
        return indexed_maintenance(maximum, false, cancel);
    ensure_directory(config_->jobs_root);
    ensure_index(*maintenance_, config_->jobs_root);
    auto summary = empty_summary();
    std::vector<std::string> ids;
    {
        std::lock_guard lock(maintenance_->mutex);
        auto& state = *maintenance_;
        const auto size = state.ids.size();
        if (!state.cycle_started) {
            ++state.cycle;
            state.cycle_started = Clock::now();
        }
        if (state.cursor >= size)
            state.cursor = 0;
        const auto end = state.cursor + std::min(maximum, size - state.cursor);
        ids.assign(state.ids.begin() + static_cast<std::ptrdiff_t>(state.cursor),
                   state.ids.begin() + static_cast<std::ptrdiff_t>(end));
        const auto complete = end >= size;
        summary["discovered"] = size;
        summary["scanned"] = ids.size();
        summary["batchLimited"] = !complete;
        summary["cycleCompleted"] = complete;
        summary["maintenanceCycle"] = state.cycle;
        summary["maintenanceCursor"] = end;
        summary["maintenanceTotal"] = size;
        summary["maintenanceProgressPercent"] = size ? end * 100 / size : 100;
        summary["maintenanceCycleAgeMs"] =
            std::chrono::duration_cast<Millis>(Clock::now() - *state.cycle_started).count();
        state.cursor = complete ? 0 : end;
        if (complete)
            state.cycle_started.reset();
    }
    std::set<std::string> deleted;
    for (const auto& id : ids) {
        try {
            const auto status = get_status(id);
            const auto name = json_string(status, "status");
            if (name == "interrupted")
                increment(summary, "interrupted");
            else if (terminal_status(name))
                increment(summary, "terminal");
            else {
                increment(summary, "active");
                continue;
            }
            const auto path = paths(id);
            const auto completed = completed_time(status);
            const auto now = static_cast<std::int64_t>(unix_millis());
            if (config_->job_retention_hours && completed > 0 && now >= completed &&
                static_cast<std::uint64_t>(now - completed) / 3600000 >= config_->job_retention_hours &&
                remove_job_directory(config_->jobs_root, path)) {
                deleted.insert(id);
                increment(summary, "deleted");
                continue;
            }
            auto raw = read_status_raw(id);
            if (raw.contains("maintenanceReconciledAtUtc"))
                continue;
            const bool out = compact_log(path.stdout_log, config_->job_log_max_bytes),
                       err = compact_log(path.stderr_log, config_->job_log_max_bytes);
            if (out || err)
                increment(summary, "compactedLogs");
            raw["maintenanceReconciledAtUtc"] = utc_now();
            raw["legacyLogsCompacted"] = out || err;
            write_status(id, raw);
            increment(summary, "maintained");
        } catch (...) {
            increment(summary, "errors");
        }
    }
    note_deleted(*maintenance_, deleted);
    summary.update(quota_snapshot());
    return summary;
}
Json JobStore::enforce_store_quota(const Cancel& cancel) {
    if (indexed())
        return indexed_maintenance(64, true, cancel);
    ensure_directory(config_->jobs_root);
    ensure_index(*maintenance_, config_->jobs_root);
    auto summary = empty_summary();
    std::vector<std::string> ids;
    bool full;
    {
        std::lock_guard lock(maintenance_->mutex);
        full = !maintenance_->quota_initialized || !maintenance_->quota_refreshed ||
               Clock::now() - *maintenance_->quota_refreshed >= std::chrono::hours(1);
        summary["discovered"] = maintenance_->ids.size();
        if (full)
            ids = maintenance_->ids;
        else
            for (const auto& id : maintenance_->ids) {
                const auto found = maintenance_->quota_entries.find(id);
                if (found == maintenance_->quota_entries.end() || !found->second.terminal)
                    ids.push_back(id);
            }
    }
    std::map<std::string, Maintenance::Entry> updates;
    std::set<std::string> vanished;
    for (const auto& id : ids) {
        const auto path = paths(id);
        if (!fs::exists(path.dir)) {
            vanished.insert(id);
            continue;
        }
        const auto bytes = directory_bytes(path.dir);
        std::optional<Json> status;
        try {
            status = read_status_raw(id);
        } catch (...) {
        }
        updates[id] = {bytes, status && terminal_status(json_string(*status, "status")),
                       status ? completed_time(*status) : 0};
    }
    std::map<std::string, Maintenance::Entry> entries;
    {
        std::lock_guard lock(maintenance_->mutex);
        if (full)
            maintenance_->quota_entries.clear();
        for (const auto& id : vanished)
            maintenance_->quota_entries.erase(id);
        for (const auto& [id, entry] : updates)
            maintenance_->quota_entries[id] = entry;
        maintenance_->quota_initialized = true;
        if (full)
            maintenance_->quota_refreshed = Clock::now();
        entries = maintenance_->quota_entries;
    }
    summary["quotaFullReconcile"] = full;
    summary["quotaScanned"] = full ? entries.size() : ids.size();
    std::uint64_t bytes = 0, terminal_bytes = 0;
    std::vector<std::pair<std::string, Maintenance::Entry>> terminal;
    for (const auto& [id, entry] : entries) {
        bytes += entry.bytes;
        if (entry.terminal) {
            terminal_bytes += entry.bytes;
            terminal.emplace_back(id, entry);
        }
    }
    auto retained = terminal.size();
    const bool pressure =
        (config_->job_store_max_bytes && bytes > config_->job_store_max_bytes) ||
        (config_->job_store_max_terminal_jobs && retained > config_->job_store_max_terminal_jobs);
    if (pressure) {
        const auto active_bytes = bytes - terminal_bytes;
        const auto low_water =
            config_->job_store_max_bytes / 10 * 9 + config_->job_store_max_bytes % 10 * 9 / 10;
        const auto target_total = active_bytes < low_water ? low_water : config_->job_store_max_bytes;
        const auto target_bytes = !config_->job_store_max_bytes ? UINT64_MAX
                                  : active_bytes >= config_->job_store_max_bytes
                                      ? terminal_bytes
                                      : target_total - active_bytes;
        const auto target_jobs = config_->job_store_max_terminal_jobs * 9 / 10;
        std::sort(terminal.begin(), terminal.end(), [](const auto& a, const auto& b) {
            return a.second.completed == b.second.completed ? a.first < b.first
                                                            : a.second.completed < b.second.completed;
        });
        std::set<std::string> deleted;
        for (const auto& [id, entry] : terminal) {
            if ((!config_->job_store_max_bytes || terminal_bytes <= target_bytes) &&
                (!config_->job_store_max_terminal_jobs || retained <= target_jobs))
                break;
            try {
                if (remove_job_directory(config_->jobs_root, paths(id))) {
                    bytes -= std::min(bytes, entry.bytes);
                    terminal_bytes -= std::min(terminal_bytes, entry.bytes);
                    if (retained)
                        --retained;
                    deleted.insert(id);
                    increment(summary, "deleted");
                    increment(summary, "quotaDeleted");
                }
            } catch (...) {
                increment(summary, "errors");
            }
        }
        note_deleted(*maintenance_, deleted);
    }
    {
        std::lock_guard lock(maintenance_->mutex);
        maintenance_->store_bytes = bytes;
        maintenance_->terminal_retained = retained;
        maintenance_->quota_pressure = pressure;
        maintenance_->quota_checked = Clock::now();
        maintenance_->quota_checked_utc = utc_now();
    }
    summary.update(quota_snapshot());
    return summary;
}
} // namespace devbox
