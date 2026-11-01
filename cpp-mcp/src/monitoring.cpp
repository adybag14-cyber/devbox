#include "devbox/monitoring.hpp"
#include "devbox/contract.hpp"
#include "devbox/management.hpp"
#include "devbox/result.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#ifndef _WIN32
#include <fcntl.h>
#endif
namespace devbox {
namespace {
constexpr std::uint64_t minimum_free = 512ULL * 1024 * 1024, warning_free = 50ULL * 1024 * 1024 * 1024;
std::uint64_t elapsed_ms(Clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Millis>(Clock::now() - started).count());
}
void writable_probe(const fs::path& root, const char* label) {
    ensure_directory(root);
    const auto path = root / path_from_utf8(std::string(".mcp-ready-") + label + "-" +
                                            std::to_string(process_id()) + "-" + uuid() + ".tmp");
    ScopeExit remove([&] {
        std::error_code ec;
        fs::remove(path, ec);
    });
#ifdef _WIN32
    NativeHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!file || !FlushFileBuffers(file.get()))
        throw Error("readiness probe " + path_text(path) + ": " + windows_error());
#else
    NativeHandle file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!file || ::fsync(file.get()) != 0)
        throw Error("readiness probe failed: " + path_text(path));
#endif
    file.reset();
    fs::remove(path);
    remove.disarm();
}
Json nullable_field(const Json& value, std::string_view name) {
    return value.contains(std::string(name)) ? value[std::string(name)] : Json();
}
} // namespace
std::optional<std::uint64_t> snapshot_age(const Json& value, std::string_view key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string())
        return {};
    const auto stamp = parse_utc(found->get_ref<const std::string&>());
    if (!stamp)
        return {};
    const auto now = unix_millis();
    return *stamp < 0 ? now : now - std::min(now, static_cast<std::uint64_t>(*stamp));
}
Json read_status_snapshot(const fs::path& path, std::uint64_t stale_ms) {
    try {
        auto value = read_json_optional(path);
        if (!value)
            return nullptr;
        const auto age = snapshot_age(*value);
        if (value->is_object()) {
            (*value)["ageMs"] = age ? Json(*age) : Json();
            (*value)["stale"] = !age || *age > stale_ms;
        }
        return *value;
    } catch (...) {
        return nullptr;
    }
}
Json guardian_snapshot(const Config& config) {
    if (env_bool("DEVBOX_NATIVE_MANAGED", false)) {
        try {
            const auto root = config.project_root / "run" / "native";
            const auto state = read_json(root / "status.json", 65536);
            const auto heartbeat = read_json(root / "heartbeat.json", 65536);
            const auto age = snapshot_age(heartbeat, "updatedAt");
            const auto& owner = state.at("owner");
            const auto pid = json_uint(owner, "pid"), instance = management_instance(owner);
            const bool alive = pid > 0 && pid <= UINT32_MAX && instance > 0 &&
                               process_matches_instance(static_cast<std::uint32_t>(pid), instance);
            const bool stale = !age || *age > 15000 || !alive ||
                               json_string(heartbeat, "epoch") != json_string(state, "epoch");
            Json reasons = Json::array();
            if (stale)
                reasons.push_back("native supervisor heartbeat is stale or its owner exited");
            if (state.contains("error"))
                reasons.push_back(state.at("error"));
            return Json{{"implementation", "cpp"},
                        {"nativeManagementVersion", 1},
                        {"observedAtUtc", heartbeat.at("updatedAt")},
                        {"ageMs", age ? Json(*age) : Json()},
                        {"stale", stale},
                        {"isHealthy", !stale && json_bool(state, "healthy")},
                        {"phase", state.value("phase", Json())},
                        {"reasons", reasons},
                        {"publicTunnelHealthy", state.value("publicHealthy", Json())},
                        {"companions", state.value("companions", Json::array())}};
        } catch (...) {
            return Json{{"implementation", "cpp"}, {"stale", true}, {"isHealthy", false}};
        }
    }
    Json value;
    try {
        value = read_json(config.project_root / "run" / "guardian" / "state.json");
    } catch (...) {
        return nullptr;
    }
    const auto age = snapshot_age(value, "ObservedAtUtc");
    const bool stale = !age || *age > 30000;
    auto reasons = value.value("Reasons", Json::array());
    if (stale && reasons.is_array())
        reasons.push_back("guardian state snapshot is stale");
    Json result{{"observedAtUtc", nullable_field(value, "ObservedAtUtc")},
                {"ageMs", age ? Json(*age) : Json()},
                {"stale", stale},
                {"isHealthy", stale ? Json(false) : nullable_field(value, "IsHealthy")},
                {"reasons", reasons}};
    for (const auto* key :
         {"NeedsRepair", "McpElevated", "PublicTunnelHealthy", "CloudflaredRunning", "CloudflaredMetrics",
          "CloudflaredMetricsDelta", "TunnelTransportHealthy", "Readiness"}) {
        auto output = std::string(key);
        output[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(output[0])));
        result[output] = nullable_field(value, key);
    }
    result["tunnelTransportDegraded"] = value.value("TunnelTransportDegraded", Json(false));
    result["tunnelTransportReasons"] = value.value("TunnelTransportReasons", Json::array());
    return result;
}
Json degraded_background(const Json& snapshot) {
    Json result = Json::array();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it)
        if (json_uint(it.value(), "consecutiveFailures") > 0 ||
            (!json_bool(it.value(), "running") &&
             !nullable_field(it.value(), "lastSuccessUnixMs").is_number()))
            result.push_back(it.key());
    return result;
}
void record_scheduler_probe_failure(Json& cache, std::uint64_t duration_ms, std::string error) {
    if (!cache.is_object())
        cache = Json{{"sampledAtUtc", nullptr}, {"snapshot", nullptr}};
    // The failed probe supplies no new queue data. Preserve the original
    // snapshot and its timestamp so repeated failures cannot renew its age.
    cache["ok"] = false;
    cache["probeSampledAtUtc"] = utc_now();
    cache["durationMs"] = duration_ms;
    cache["error"] = std::move(error);
}
Json scheduler_status_summary(const Json& cache) {
    const auto age = snapshot_age(cache);
    const auto snapshot = cache.find("snapshot");
    const bool has_sample = snapshot != cache.end() && snapshot->is_object();
    const bool available = has_sample && age && *age < 5000;
    Json::object_t result;
    result.reserve(20);
    for (const auto* name :
         {"max_concurrent", "occupied", "global_queued", "global_queued_by_class", "reserved_interactive",
          "heavy_capacity", "io_heavy_capacity", "background_capacity", "watch_capacity", "watch_occupied",
          "resource_capacity", "resource_capacity_policy"}) {
        if (available) {
            const auto value = snapshot->find(name);
            result.emplace_back(name, value == snapshot->end() ? Json() : *value);
        } else {
            result.emplace_back(name, nullptr);
        }
    }
    result.emplace_back("sampledAtUtc", nullable_field(cache, "sampledAtUtc"));
    result.emplace_back("probeSampledAtUtc", nullable_field(cache, "probeSampledAtUtc"));
    result.emplace_back("ageMs", age ? Json(*age) : Json());
    result.emplace_back("stale", !available);
    result.emplace_back("available", available);
    result.emplace_back("ok", json_bool(cache, "ok"));
    result.emplace_back("error", nullable_field(cache, "error"));
    result.emplace_back("unavailableReason",
                        available ? Json() : Json(!has_sample || !age ? "not_sampled" : "expired"));
    return Json(std::move(result));
}
bool disk_space_sample_valid(const fs::space_info& space, bool query_succeeded) {
    const auto unknown = std::numeric_limits<std::uintmax_t>::max();
    return query_succeeded && space.capacity != unknown && space.available != unknown &&
           space.capacity != 0 && space.available <= space.capacity;
}
std::uint64_t disk_warning_free_bytes(std::uint64_t total) {
    // A fixed 50 GiB warning is permanently active on a smaller filesystem.
    // Keep the hard floor and the established large-volume byte allowance.
    const auto capacity_allowance = total / 10 + (total % 10 != 0 ? 1 : 0);
    return std::max(minimum_free, std::min(warning_free, capacity_allowance));
}
std::string disk_pressure(std::uint64_t free, std::uint64_t total, bool valid) {
    if (!valid || !total || free > total || free < minimum_free)
        return "critical";
    // ceil(total / 20) preserves the strict 5% rule without multiplication
    // overflow or floating-point rounding at large uint64_t capacities.
    const auto percent_warning = total / 20 + (total % 20 != 0 ? 1 : 0);
    return free < disk_warning_free_bytes(total) || free < percent_warning ? "warning" : "normal";
}
bool cleanup_operation(std::string_view operation) {
    const auto raw = lower(trim(operation));
    if (raw.empty() || raw.find_first_of(";&|\r\n") != raw.npos)
        return false;
    std::istringstream words(raw);
    std::vector<std::string> list;
    std::string word;
    while (words >> word)
        list.push_back(word);
    const auto text = join(list, " ");
    if (text.starts_with("rm -"))
        return true;
    for (const auto* item :
         {"remove-item", "rmdir", "del", "git clean", "cargo clean", "npm cache clean", "npm cache verify",
          "pip cache purge", "docker system prune", "docker builder prune", "docker image prune",
          "docker container prune", "docker volume prune", "wsl --shutdown"})
        if (text == item || text.starts_with(std::string(item) + " "))
            return true;
    return false;
}
OperationalMonitor::OperationalMonitor(std::shared_ptr<const Config> config, BackgroundTasks& background,
                                       ExecutionScheduler& scheduler, JobStore& jobs,
                                       RuntimeExecutor& runtime, PerformanceMonitor& performance,
                                       UsageTelemetry& usage, std::function<std::size_t()> active_requests)
    : config_(std::move(config)), background_(background), scheduler_(scheduler), jobs_(jobs),
      runtime_(runtime), performance_(performance), usage_(usage),
      active_requests_(std::move(active_requests)),
      incidents_(config_->project_root / "run" / "mcp-incidents.jsonl", 4 * 1024 * 1024, 3) {}
void OperationalMonitor::start() {
    background_.adaptive("execution-store-probe", Millis(0), [this](const Cancel& cancel) {
        cancel->check();
        probe_store();
        return Millis(60000);
    });
    background_.periodic("scheduler-snapshot", Millis(25), Millis(1000),
                         [this](const Cancel&) { scheduler_iteration(); });
    background_.periodic("job-maintenance", Millis(17000), Millis(60000),
                         [this](const Cancel& cancel) { job_iteration(false, cancel); });
    background_.adaptive("job-quota", Millis(0),
                         [this, first = std::make_shared<std::atomic_bool>(true)](const Cancel& cancel) {
                             job_iteration(true, cancel);
                             if (json_bool(jobs_.quota_snapshot(), "quotaCyclePending"))
                                 return Millis(100);
                             return first->exchange(false) ? Millis(37000) : Millis(60000);
                         });
    background_.periodic("incident-monitor", Millis(7000), Millis(10000),
                         [this](const Cancel&) { incident_iteration(); });
    background_.periodic(
        "version-refresh", Millis(0),
        Millis(std::clamp<std::uint64_t>(config_->devbox_version_cache_ms / 2, 30000, 120000)),
        [this](const Cancel& cancel) { runtime_.get_versions(true, cancel); });
}
void OperationalMonitor::probe_store() {
    const auto started = Clock::now();
    std::vector<std::string> errors;
    auto probe = [&](const fs::path& path, const char* label) {
        try {
            writable_probe(path, label);
            return true;
        } catch (const std::exception& e) {
            errors.push_back(e.what());
            return false;
        }
    };
    const bool jobs = probe(config_->jobs_root, "jobs"), slots = probe(config_->execution_slot_root, "slots");
    std::error_code ec;
    const auto space = fs::space(config_->jobs_root, ec);
    const auto free = ec ? 0 : space.available, total = ec ? 0 : space.capacity;
    const bool valid_space = disk_space_sample_valid(space, !ec);
    if (ec)
        errors.push_back(ec.message());
    else if (!valid_space)
        errors.push_back("disk capacity or available-space measurement is invalid");
    else if (free < minimum_free)
        errors.push_back("free disk " + std::to_string(free) + " below minimum " +
                         std::to_string(minimum_free));
    const auto pressure = disk_pressure(free, total, valid_space);
    Json trend = nullptr;
    std::size_t samples = 0;
    std::uint64_t window = 0;
    {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        disk_samples_.push_back({now, free});
        while (!disk_samples_.empty() && now - disk_samples_.front().first > std::chrono::minutes(30))
            disk_samples_.pop_front();
        samples = disk_samples_.size();
        window = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - disk_samples_.front().first).count());
        if (samples >= 5 && window >= 180) {
            std::vector<long double> slopes;
            for (std::size_t i = 1; i < samples; ++i) {
                const auto& a = disk_samples_[i - 1];
                const auto& b = disk_samples_[i];
                const auto dt = std::chrono::duration_cast<Millis>(b.first - a.first).count();
                if (dt >= 1000)
                    slopes.push_back(
                        std::trunc((static_cast<long double>(b.second) - static_cast<long double>(a.second)) *
                                   3600000.0L / dt));
            }
            if (slopes.size() >= 4) {
                std::sort(slopes.begin(), slopes.end());
                const auto middle = slopes.size() / 2;
                auto median = (slopes.size() % 2) ? slopes[middle]
                                                  : std::trunc((slopes[middle - 1] + slopes[middle]) / 2);
                median =
                    std::clamp(median, static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
                               static_cast<long double>(std::numeric_limits<std::int64_t>::max()));
                // A double-width long double is used where available; clamp before conversion on Windows.
                if (median >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                    trend = std::numeric_limits<std::int64_t>::max();
                else
                    trend = static_cast<std::int64_t>(median);
            }
        }
    }
    const double percent =
        total ? static_cast<double>(
                    std::min(10000.0L, std::floor(static_cast<long double>(free) * 10000 / total))) /
                    100
              : 0;
    Json value{{"ok", errors.empty()},
               {"sampledAtUtc", utc_now()},
               {"durationMs", elapsed_ms(started)},
               {"jobsWritable", jobs},
               {"schedulerWritable", slots},
               {"freeBytes", free},
               {"totalBytes", total},
               {"freePercent", percent},
               {"freeBytesTrendPerHour", trend},
               {"freeBytesTrendSampleCount", samples},
               {"freeBytesTrendWindowSeconds", window},
               {"diskPressure", pressure},
               {"warningFreeBytes", disk_warning_free_bytes(total)},
               {"warningFreePercent", 5.0},
               {"minimumFreeBytes", minimum_free},
               {"error", errors.empty() ? Json() : Json(join(errors, "; "))}};
    try {
        write_json_atomic(config_->execution_slot_root / ".disk-pressure.json",
                          Json{{"diskPressure", pressure},
                               {"freeBytes", free},
                               {"totalBytes", total},
                               {"sampledAtUtc", utc_now()}});
    } catch (const std::exception& e) {
        errors.push_back(e.what());
        value["ok"] = false;
        value["error"] = join(errors, "; ");
    }
    {
        std::lock_guard lock(mutex_);
        store_ = value;
    }
    if (!errors.empty())
        throw Error(join(errors, "; "));
}
void OperationalMonitor::scheduler_iteration() {
    const auto start = Clock::now();
    try {
        auto snapshot = scheduler_.snapshot();
        std::lock_guard lock(mutex_);
        const auto sampled_at = utc_now();
        execution_ = Json{{"ok", true},
                          {"sampledAtUtc", sampled_at},
                          {"probeSampledAtUtc", sampled_at},
                          {"durationMs", elapsed_ms(start)},
                          {"snapshot", std::move(snapshot)},
                          {"error", nullptr}};
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        record_scheduler_probe_failure(execution_, elapsed_ms(start), e.what());
        throw;
    }
}
void OperationalMonitor::job_iteration(bool quota, const Cancel& cancel) {
    const auto start = Clock::now();
    Json value{{"sampledAtUtc", utc_now()}};
    std::optional<std::string> failure;
    try {
        value["summary"] =
            quota ? jobs_.enforce_store_quota(cancel) : jobs_.reconcile_maintenance(100, cancel);
    } catch (const std::exception& e) {
        failure = e.what();
        value["error"] = *failure;
    }
    value["durationMs"] = elapsed_ms(start);
    write_json_atomic(config_->project_root / "run" / (quota ? "job-quota.json" : "job-maintenance.json"),
                      value);
    if (failure)
        throw Error(*failure);
}
void OperationalMonitor::incident_iteration() {
    const auto performance = performance_.snapshot();
    const auto event = performance.value("eventLoop", Json::object());
    const bool readiness = ready(cpp_tool_count);
    const bool changed = readiness != last_incident_ready_;
    const bool delayed = json_number(event, "p95Ms") > 100 || json_number(event, "maxMs") > 500 ||
                         json_number(event, "timerDriftMaxMs") > 250;
    if (readiness && !changed && !delayed)
        return;
    // Record degradation/recovery even inside a prior performance incident's
    // cooldown. Repeated unchanged failures retain the existing rate limit.
    if (!changed && Clock::now() - last_incident_ < Millis(30000))
        return;
    const auto cache = execution_cache();
    incidents_.append(Json{{"observedAtUtc", utc_now()},
                           {"trigger",
                            {{"p95Ms", json_number(event, "p95Ms")},
                             {"maxMs", json_number(event, "maxMs")},
                             {"timerDriftMaxMs", json_number(event, "timerDriftMaxMs")},
                             {"ready", readiness},
                             {"readinessChanged", changed}}},
                           {"build", build_snapshot()},
                           {"performance", performance},
                           {"execution", nullable_field(cache, "snapshot")},
                           {"executionSnapshot",
                            {{"sampledAtUtc", nullable_field(cache, "sampledAtUtc")},
                             {"ok", nullable_field(cache, "ok")},
                             {"error", nullable_field(cache, "error")}}},
                           {"activeRequests", active_requests_()},
                           {"activeTools", usage_.active_tools()},
                           {"backgroundTasks", background_.snapshot()},
                           {"executionStore", store_health()}});
    last_incident_ = Clock::now();
    last_incident_ready_ = readiness;
}
Json OperationalMonitor::execution_cache() const {
    std::lock_guard lock(mutex_);
    return execution_;
}
std::optional<Json> OperationalMonitor::execution(bool summary) const {
    std::lock_guard lock(mutex_);
    if (summary)
        return scheduler_status_summary(execution_);
    const auto age = snapshot_age(execution_);
    const auto snapshot = execution_.find("snapshot");
    if (!age || *age >= 5000 || snapshot == execution_.end() || snapshot->is_null())
        return {};
    return std::optional<Json>(std::in_place, *snapshot);
}
Json OperationalMonitor::store_health() const {
    std::lock_guard lock(mutex_);
    return store_;
}
bool OperationalMonitor::ready(std::size_t tools) const {
    if (tools != cpp_tool_count)
        return false;
    {
        std::lock_guard lock(mutex_);
        const auto age = snapshot_age(execution_);
        const auto snapshot = execution_.find("snapshot");
        if (!age || *age >= 5000 || snapshot == execution_.end() || snapshot->is_null())
            return false;
    }
    // Read the one worker's typed state without constructing diagnostic JSON.
    // Keep the monitor and background locks separate, as in the snapshot path.
    if (!background_.healthy("execution-store-probe", 150000))
        return false;
    std::lock_guard lock(mutex_);
    const auto age = snapshot_age(store_);
    return json_bool(store_, "ok") && age && *age < 150000;
}
std::optional<Json> OperationalMonitor::reject_disk_work(ResourceClass resource, bool read_only,
                                                         std::string_view operation) const {
    if (read_only || (resource != ResourceClass::heavy && resource != ResourceClass::io_heavy) ||
        cleanup_operation(operation))
        return {};
    const auto health = store_health();
    if (json_string(health, "diskPressure") != "critical")
        return {};
    return result_error("Disk pressure is critical; refusing new storage-intensive mutating work. Read-only "
                        "inspection and cleanup operations remain available.",
                        Json{{"diskPressure", "critical"},
                             {"freeBytes", nullable_field(health, "freeBytes")},
                             {"resourceClass", resource_name(resource)},
                             {"recovery", "Free disk space or run a cleanup operation, then retry."}});
}
} // namespace devbox
