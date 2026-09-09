#include "devbox/jobs.hpp"
#include "jobs_internal.hpp"
#include <algorithm>
#include <thread>
#ifndef _WIN32
#include <signal.h>
#endif
namespace devbox {
namespace {
std::optional<std::uint32_t> pid_field(const Json& value, std::string_view field) {
    try {
        const auto pid = json_uint(value, field);
        if (pid && pid <= UINT32_MAX)
            return static_cast<std::uint32_t>(pid);
    } catch (...) {
    }
    return std::nullopt;
}
std::optional<std::uint64_t> instance_field(const Json& value, std::string_view field) {
    if (!value.contains(std::string(field)))
        return std::nullopt;
    return owner_process_instance(Json{{"processInstance", value[std::string(field)]}});
}
bool runner_alive(const Json& value) {
    const auto pid = pid_field(value, "runnerPid");
    return pid && process_matches_instance(*pid, instance_field(value, "runnerProcessInstance"));
}
std::optional<Millis> file_age(const fs::path& path) {
    std::error_code ec;
    const auto modified = fs::last_write_time(path, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return std::nullopt;
    if (ec)
        throw std::system_error(ec);
    const auto now = fs::file_time_type::clock::now();
    return now >= modified ? std::optional(std::chrono::duration_cast<Millis>(now - modified)) : std::nullopt;
}
std::optional<Json> heartbeat(const JobPaths& paths) {
    try {
        return read_json_optional(paths.heartbeat, 65536);
    } catch (...) {
        return std::nullopt;
    }
}
Millis status_age(const Json& value) {
    const auto now = static_cast<std::int64_t>(unix_millis());
    for (const auto key : {"startedAtUtc", "queuedAtUtc", "createdAtUtc"}) {
        if (const auto parsed = parse_utc(json_string(value, key)); parsed && now > *parsed)
            return Millis(now - *parsed);
    }
    return Millis::max();
}
Json decorate(Json value, const JobPaths& paths, bool alive, std::optional<Millis> age) {
    value["runnerAlive"] = alive;
    if (age)
        value["heartbeatAgeMs"] = age->count();
    value["jobDir"] = path_text(paths.dir);
    return value;
}
Json read_job_json(const fs::path& path, std::size_t maximum = 16 * 1024 * 1024) {
    std::exception_ptr failure;
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            return read_json(path, maximum);
        } catch (...) {
            failure = std::current_exception();
        }
        if (attempt < 2)
            std::this_thread::sleep_for(Millis(5));
    }
    std::rethrow_exception(failure);
}
} // namespace
bool terminal_status(std::string_view value) {
    return value == "succeeded" || value == "failed" || value == "cancelled" || value == "timed_out" ||
           value == "interrupted";
}
std::string validate_job_id(std::string_view raw) {
    const auto value = trim(raw);
    const auto alphanumeric = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (value.size() < 8 || value.size() > 81 || !alphanumeric(value.front()) ||
        !std::all_of(value.begin() + 1, value.end(), [&](char c) { return alphanumeric(c) || c == '-'; }))
        throw Error("Invalid Devbox job id.");
    return value;
}
std::vector<std::string> job_ids(const fs::path& root, std::size_t maximum) {
    std::vector<std::string> ids;
    std::error_code ec;
    fs::directory_iterator iterator(root, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return ids;
    if (ec)
        throw std::system_error(ec);
    const auto deadline = Clock::now() + Millis(5000);
    for (const auto& entry : iterator) {
        if (Clock::now() >= deadline)
            throw Error("Job discovery exceeded its five-second deadline");
        const auto name = path_text(entry.path().filename());
        if (!name.starts_with("job-") || !entry.is_directory())
            continue;
        if (ids.size() >= maximum)
            throw Error("Job store exceeds the bounded discovery budget; reconcile retention before "
                        "submitting more work");
        ids.push_back(name);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}
JobStore::JobStore(std::shared_ptr<const Config> config)
    : config_(std::move(config)), maintenance_(std::make_shared<Maintenance>()) {}
JobPaths JobStore::paths(std::string_view id) const {
    const auto name = validate_job_id(id);
    const auto dir = config_->jobs_root / path_from_utf8(name);
    return {name,
            dir,
            dir / "request.json",
            dir / "status.json",
            dir / "stdout.log",
            dir / "stderr.log",
            dir / "cancel.requested",
            dir / "heartbeat.json"};
}
JobPaths JobStore::create_job(std::string_view id, const Json& request, const Json& status) {
    const auto value = paths(id);
    ensure_directory(config_->jobs_root);
    if (!fs::create_directory(value.dir))
        throw Error("JOB_EXISTS: persisted job directory will not be overwritten");
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove_all(value.dir, ec);
    });
    write_json_atomic(value.request, request);
    write_json_atomic(value.status, status);
    write_file(value.stdout_log, "");
    write_file(value.stderr_log, "");
    {
        std::lock_guard lock(maintenance_->mutex);
        auto position = std::lower_bound(maintenance_->ids.begin(), maintenance_->ids.end(), value.id);
        if (position == maintenance_->ids.end() || *position != value.id) {
            const auto at = static_cast<std::size_t>(position - maintenance_->ids.begin());
            maintenance_->ids.insert(position, value.id);
            if (at < maintenance_->cursor)
                ++maintenance_->cursor;
        }
        if (maintenance_->quota_initialized)
            maintenance_->quota_entries[value.id] = {};
    }
    cleanup.disarm();
    return value;
}
Json JobStore::read_request(std::string_view id) const {
    return read_job_json(paths(id).request);
}
Json JobStore::read_status_raw(std::string_view id) const {
    return read_job_json(paths(id).status);
}
void JobStore::write_status(std::string_view id, const Json& status) const {
    const auto path = paths(id);
    write_json_atomic(path.status, status);
    bool initialized;
    {
        std::lock_guard lock(maintenance_->mutex);
        initialized = maintenance_->quota_initialized;
    }
    if (initialized) {
        std::uint64_t bytes = 0;
        for (const auto& entry : fs::directory_iterator(path.dir))
            if (entry.is_regular_file() && !entry.is_symlink())
                bytes += entry.file_size();
        auto completed = parse_utc(json_string(status, "completedAtUtc"));
        if (!completed)
            completed = parse_utc(json_string(status, "createdAtUtc"));
        std::lock_guard lock(maintenance_->mutex);
        maintenance_->quota_entries[path.id] = {bytes, terminal_status(json_string(status, "status")),
                                                completed.value_or(0)};
    }
}
void JobStore::write_heartbeat(std::string_view id, const Json& value) const {
    write_json_atomic(paths(id).heartbeat, value);
}
bool JobStore::cancellation_requested(std::string_view id) const {
    return fs::exists(paths(id).cancel);
}
Json JobStore::get_status(std::string_view id) const {
    const auto path = paths(id);
    return reconcile(path, read_job_json(path.status));
}
Json JobStore::reconcile(const JobPaths& paths, Json value) const {
    if (!value.is_object())
        throw Error("Job status " + path_text(paths.status) + " is not a JSON object.");
    const auto status = json_string(value, "status");
    if (terminal_status(status) && status != "cancelled")
        return decorate(std::move(value), paths, false, {});
    if (status == "cancelled" || fs::exists(paths.cancel))
        return reconcile_cancelled(paths, std::move(value));
    const auto beat = heartbeat(paths);
    const auto age = file_age(paths.heartbeat);
    const auto stale_after = Millis(std::max<std::uint64_t>(1000, config_->job_orphan_stale_ms));
    const auto stale = age ? *age >= stale_after : status_age(value) >= stale_after;
    const auto pid = pid_field(value, "runnerPid");
    const auto alive = pid && (!stale || runner_alive(value));
    if (!stale && beat && beat->contains("childPid") && !(*beat)["childPid"].is_null())
        value["childPid"] = (*beat)["childPid"];
    else if (stale && alive) {
        value["heartbeatStale"] = true;
        value["monitoringWarning"] =
            "Runner is alive but its heartbeat is stale; inspect job progress and storage health";
    }
    if (!alive && stale)
        return interrupt_orphan(paths, std::move(value), beat, age);
    return decorate(std::move(value), paths, alive, age);
}
Json JobStore::reconcile_cancelled(const JobPaths& paths, Json value) const {
    const auto alive = runner_alive(value);
    const auto beat = heartbeat(paths);
    auto child = beat ? pid_field(*beat, "childPid") : std::nullopt;
    if (!child)
        child = pid_field(value, "childPid");
    const auto instance = beat ? instance_field(*beat, "childProcessInstance") : std::nullopt;
    const auto child_alive = child && process_matches_instance(*child, instance);
    const auto docker = json_string(value, "runtimeMode") == "docker";
    const auto pending = alive || child_alive;
    if (!pending && docker) {
        value["status"] = "interrupted";
        value["completedAtUtc"] = utc_now();
        value["cancelRequested"] = true;
        value["workloadTerminationVerified"] = false;
        value["terminationDetail"] =
            "Local Docker runner stopped; termination of the workload in the shared container is unverified";
        value.erase("terminationPending");
        value.erase("childAlive");
        write_json_atomic(paths.status, value);
        return decorate(std::move(value), paths, false, {});
    }
    if (!pending && json_string(value, "status") == "cancelled")
        return decorate(std::move(value), paths, false, {});
    value["status"] = pending ? "cancel_requested" : "cancelled";
    value["cancelRequested"] = true;
    if (pending) {
        value["completedAtUtc"] = nullptr;
        value["terminationPending"] = true;
        value["childAlive"] = child_alive;
        if (docker)
            value["terminationDetail"] =
                "Local Docker client termination does not verify the in-container workload stopped";
    } else {
        if (!value.contains("completedAtUtc") || value["completedAtUtc"].is_null())
            value["completedAtUtc"] = utc_now();
        value.erase("terminationPending");
        value.erase("childAlive");
        write_json_atomic(paths.status, value);
    }
    return decorate(std::move(value), paths, alive, {});
}
Json JobStore::interrupt_orphan(const JobPaths& paths, Json value, const std::optional<Json>& beat,
                                std::optional<Millis> age) const {
    auto child = beat ? pid_field(*beat, "childPid") : std::nullopt;
    if (!child)
        child = pid_field(value, "childPid");
    auto instance = beat ? instance_field(*beat, "childProcessInstance") : std::nullopt;
    if (!instance)
        instance = instance_field(value, "childProcessInstance");
    const auto runtime =
        json_string(value, "runtimeMode", beat ? json_string(*beat, "runtimeMode", "host") : "host");
    bool child_terminated = false, docker_terminated = false;
    std::optional<std::string> skipped;
    if (child) {
        if (!process_matches_instance(*child, instance))
            skipped = "child-process-instance-no-longer-matches";
        else if (!age || *age > Millis(60000) || !instance)
            skipped = "heartbeat-too-old-to-safely-trust-reused-pid";
        else {
            terminate_process_tree(*child, instance);
            const auto terminated = !process_matches_instance(*child, instance);
            if (runtime == "docker") {
                docker_terminated = terminated;
                skipped = "docker-container-exec-not-force-killed-shared-container";
            } else
                child_terminated = terminated;
        }
    }
    value["status"] = "interrupted";
    if (!value.contains("completedAtUtc") || value["completedAtUtc"].is_null())
        value["completedAtUtc"] = utc_now();
    value["interrupted"] = true;
    value["runtimeMode"] = runtime;
    value["orphanChildTerminated"] = child_terminated;
    value["orphanDockerClientTerminated"] = docker_terminated;
    value["orphanChildCleanupSkipped"] = skipped ? Json(*skipped) : Json(nullptr);
    if (child)
        value["childPid"] = *child;
    if (!value.contains("error"))
        value["error"] = "Detached job runner disappeared before recording a terminal status.";
    value["heartbeatAgeMs"] = age ? Json(age->count()) : Json(nullptr);
    write_json_atomic(paths.status, value);
    return decorate(std::move(value), paths, false, age);
}
Json JobStore::wait_status(std::string_view id, Millis wait, bool terminal_only, Millis poll,
                           const Cancel& cancel) const {
    const auto bounded = std::max(
        Millis(0),
        std::min(wait, Millis(static_cast<std::int64_t>(std::max(0.1, config_->max_wait_seconds) * 1000))));
    auto current = get_status(id);
    if (bounded == Millis(0) || terminal_status(json_string(current, "status")))
        return current;
    const auto initial = json_string(current, "status");
    const auto started = Clock::now();
    while (Clock::now() - started < bounded) {
        const auto delay = std::max(
            Millis(1), std::min(std::max(poll, Millis(50)),
                                bounded - std::chrono::duration_cast<Millis>(Clock::now() - started)));
        if (cancel) {
            if (cancel->wait_for(delay))
                throw Error("Job status wait cancelled by the MCP client.");
        } else
            std::this_thread::sleep_for(delay);
        if (Clock::now() - started >= bounded)
            break;
        current = get_status(id);
        const auto status = json_string(current, "status");
        if (terminal_status(status) || (!terminal_only && status != initial))
            return current;
    }
    current["waitTimedOut"] = true;
    current["waitedMs"] = bounded.count();
    return current;
}
Json JobStore::logs(std::string_view id, std::size_t max_chars) const {
    const auto path = paths(id);
    const auto bounded = std::clamp<std::size_t>(max_chars, 100, 100000);
    const auto out = read_log_tail(path.stdout_log, bounded, config_->job_log_rotations),
               err = read_log_tail(path.stderr_log, bounded, config_->job_log_rotations);
    const auto status = get_status(id), out_meta = log_metadata(path.stdout_log, config_->job_log_rotations),
               err_meta = log_metadata(path.stderr_log, config_->job_log_rotations);
    return Json{
        {"id", path.id},
        {"status", json_string(status, "status")},
        {"stdout", out},
        {"stderr", err},
        {"maxChars", bounded},
        {"logs", Json{{"stdout", out_meta},
                      {"stderr", err_meta},
                      {"maxBytesPerSegment", std::max<std::uint64_t>(4096, config_->job_log_max_bytes)},
                      {"rotations", config_->job_log_rotations},
                      {"truncated", out_meta["rotated"] == true || err_meta["rotated"] == true}}}};
}
Json JobStore::cancel(std::string_view id) const {
    const auto path = paths(id);
    const auto status = get_status(id);
    const auto name = json_string(status, "status");
    const auto alive = json_bool(status, "runnerAlive");
    if (terminal_status(name) && !(name == "cancelled" && alive))
        return status;
    auto acknowledgement = status;
    acknowledgement["cancelRequested"] = true;
    if (!fs::exists(path.cancel))
        atomic_write(path.cancel, utc_now() + '\n', false, false, {"missing", {}});
    const auto pid = pid_field(status, "runnerPid");
    const auto instance = instance_field(status, "runnerProcessInstance");
    if (pid && *pid != process_id() && instance && alive && runner_alive(status)) {
#ifndef _WIN32
        ::kill(static_cast<pid_t>(*pid), SIGTERM);
        const auto graceful_deadline = Clock::now() + Millis(3000);
        while (Clock::now() < graceful_deadline && process_matches_instance(*pid, instance)) {
            const auto value = read_status_raw(id);
            if (terminal_status(json_string(value, "status")) && value.contains("logs"))
                break;
            std::this_thread::sleep_for(Millis(50));
        }
#endif
        if (process_matches_instance(*pid, instance))
            terminate_process_tree(*pid, instance);
    }
    const auto deadline = Clock::now() + Millis(2000);
    while (true) {
        auto current = get_status(id);
        const auto state = json_string(current, "status");
        if (state != "cancel_requested" || Clock::now() >= deadline) {
            if (state == "cancelled" && !json_bool(current, "runnerAlive")) {
                acknowledgement["status"] = "cancelled";
                acknowledgement["runnerAlive"] = false;
                acknowledgement["completedAtUtc"] = current["completedAtUtc"];
                acknowledgement.erase("terminationPending");
                acknowledgement.erase("childAlive");
                return acknowledgement;
            }
            if (status.contains("heartbeatAgeMs") && !current.contains("heartbeatAgeMs"))
                current["heartbeatAgeMs"] = status["heartbeatAgeMs"];
            return current;
        }
        std::this_thread::sleep_for(Millis(50));
    }
}
void JobStore::admit(const std::optional<std::string>& task) const {
    const auto deadline = Clock::now() + Millis(5000);
    std::size_t active = 0, task_active = 0;
    for (const auto& id : job_ids(config_->jobs_root)) {
        if (Clock::now() >= deadline)
            throw Error("Job admission inspection exceeded its deadline");
        const auto status = get_status(id);
        if (!terminal_status(json_string(status, "status"))) {
            ++active;
            if (task) {
                const auto request = read_request(id);
                if (request.contains("agent") && json_string(request["agent"], "taskId") == *task)
                    ++task_active;
            }
        }
        if (active >= config_->job_max_active || (task && task_active >= config_->job_max_per_task))
            throw Error("JOB_CAPACITY: active or queued runner limit reached; retry the same operation after "
                        "a job completes");
    }
}
Json JobStore::list(const std::optional<std::string>& task, const std::vector<std::string>& statuses,
                    const std::optional<std::string>& cursor, std::size_t limit) const {
    if (limit < 1 || limit > 100)
        throw Error("limit must be between 1 and 100");
    if (task)
        validate_key(*task);
    const auto deadline = Clock::now() + Millis(5000);
    Json jobs = Json::array();
    Json next = nullptr;
    for (const auto& id : job_ids(config_->jobs_root)) {
        if (Clock::now() >= deadline)
            throw Error("Job discovery exceeded its five-second deadline");
        if (cursor && id <= *cursor)
            continue;
        const auto status = get_status(id), request = read_request(id);
        if (task && (!request.contains("agent") || json_string(request["agent"], "taskId") != *task))
            continue;
        if (!statuses.empty() &&
            std::find(statuses.begin(), statuses.end(), json_string(status, "status")) == statuses.end())
            continue;
        if (jobs.size() >= limit) {
            next = jobs.back()["id"];
            break;
        }
        Json summary{{"id", id}};
        for (const auto key :
             {"status", "createdAtUtc", "startedAtUtc", "completedAtUtc", "exitCode", "runnerAlive"})
            if (status.contains(key))
                summary[key] = status[key];
        if (request.contains("agent"))
            summary["agent"] = request["agent"];
        jobs.push_back(std::move(summary));
    }
    return Json{{"jobs", jobs}, {"next_cursor", next}, {"order", "job_id"}, {"limit", limit}};
}
} // namespace devbox
