#include "devbox/scheduler.hpp"
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#endif
namespace devbox {
ResourceClass resource_class(std::string_view value) {
    const auto name = lower(trim(value));
    return name == "watch"                                                 ? ResourceClass::watch
           : name == "heavy"                                               ? ResourceClass::heavy
           : name == "io-heavy" || name == "io_heavy" || name == "ioheavy" ? ResourceClass::io_heavy
                                                                           : ResourceClass::light;
}
std::string resource_name(ResourceClass value) {
    switch (value) {
    case ResourceClass::watch:
        return "watch";
    case ResourceClass::heavy:
        return "heavy";
    case ResourceClass::io_heavy:
        return "io-heavy";
    default:
        return "light";
    }
}
std::string execution_name(ExecutionKind value) {
    return value == ExecutionKind::background ? "background" : "interactive";
}
SchedulerConfig SchedulerConfig::from(const Config& config) {
    return {config.execution_slot_root,
            config.exec_max_concurrent,
            config.exec_reserved_interactive,
            config.watch_max_concurrent,
            Millis(config.exec_queue_timeout_ms),
            config.exec_heavy_capacity,
            config.exec_heavy_weight,
            config.exec_io_heavy_capacity,
            config.exec_io_heavy_weight,
            Millis(config.background_priority_age_ms)};
}
SchedulerConfig SchedulerConfig::normalized() const {
    auto out = *this;
    out.max_concurrent = std::max<std::size_t>(1, max_concurrent);
    out.reserved_interactive = std::min(reserved_interactive, out.max_concurrent - 1);
    out.watch_max_concurrent = std::max<std::size_t>(1, watch_max_concurrent);
    out.queue_timeout = std::max(queue_timeout, Millis(1));
    out.heavy_weight = std::clamp<std::size_t>(heavy_weight, 1, out.max_concurrent);
    out.heavy_capacity = std::clamp(heavy_capacity, out.heavy_weight, out.max_concurrent);
    out.io_heavy_weight = std::clamp<std::size_t>(io_heavy_weight, 1, out.max_concurrent);
    out.io_heavy_capacity = std::clamp(io_heavy_capacity, out.io_heavy_weight, out.max_concurrent);
    out.background_priority_age = std::max(background_priority_age, Millis(1));
    return out;
}
std::optional<std::uint64_t> owner_process_instance(const Json& owner) {
    try {
        if (!owner.contains("processInstance"))
            return std::nullopt;
        const auto& value = owner["processInstance"];
        if (value.is_number_unsigned())
            return value.get<std::uint64_t>();
        if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
            return value.get<std::uint64_t>();
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            std::size_t count = 0;
            const auto result = std::stoull(text, &count);
            if (count == text.size())
                return result;
        }
    } catch (...) {
    }
    return std::nullopt;
}
bool owner_process_alive(const Json& owner) {
    try {
        const auto pid = json_uint(owner, "pid");
        return pid > 0 && pid <= UINT32_MAX &&
               process_matches_instance(static_cast<std::uint32_t>(pid), owner_process_instance(owner));
    } catch (...) {
        return false;
    }
}
struct SchedulerMetrics {
    struct Class {
        std::uint64_t active = 0, acquired = 0, total_wait = 0, max_wait = 0;
    };
    std::mutex mutex;
    std::uint64_t queued = 0, active = 0, acquired = 0, timed_out = 0, cancelled = 0, total_wait = 0,
                  max_wait = 0;
    std::map<std::string, Class> classes;
    Json snapshot() {
        std::lock_guard lock(mutex);
        Json by_class = Json::object();
        for (const auto& [name, v] : classes)
            by_class[name] = Json{{"active", v.active},
                                  {"acquired", v.acquired},
                                  {"average_queue_wait_ms", v.acquired ? v.total_wait / v.acquired : 0},
                                  {"max_queue_wait_ms", v.max_wait}};
        return Json{{"queued", queued},
                    {"active", active},
                    {"acquired", acquired},
                    {"timed_out", timed_out},
                    {"cancelled", cancelled},
                    {"average_queue_wait_ms", acquired ? total_wait / acquired : 0},
                    {"max_queue_wait_ms", max_wait},
                    {"by_resource_class", by_class}};
    }
};
namespace {
struct OwnedFile {
    fs::path path;
    std::string token;
    std::size_t index = 0;
};
std::uint64_t elapsed_ms(Clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Millis>(Clock::now() - started).count());
}
Json instance_json() {
    const auto value = process_instance(process_id());
    return value ? Json(std::to_string(*value)) : Json(nullptr);
}
std::optional<Json> inspect(const fs::path& path) {
    try {
        const auto value = read_json_optional(path, 65536);
        return value && value->is_object() ? value : std::nullopt;
    } catch (const Json::exception&) {
        return std::nullopt;
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory)
            return std::nullopt;
        throw;
    }
}
bool fresh(const fs::path& path, Millis limit) {
    std::error_code ec;
    const auto modified = fs::last_write_time(path, ec);
    if (ec)
        return false;
    const auto now = fs::file_time_type::clock::now();
    return modified > now || now - modified < limit;
}
bool remove_file(const fs::path& path) {
    std::error_code ec;
    const auto removed = fs::remove(path, ec);
    if (!ec || ec == std::errc::no_such_file_or_directory)
        return removed;
#ifdef _WIN32
    if (ec == std::errc::permission_denied || ec.value() == ERROR_SHARING_VIOLATION)
        return false;
#endif
    throw std::system_error(ec, "release execution state");
}
void release_owned(const OwnedFile& owned) {
    const auto current = inspect(owned.path);
    if (current && json_string(*current, "token") == owned.token && !remove_file(owned.path) &&
        fs::exists(owned.path))
        throw Error("Failed to release execution-slot file: " + path_text(owned.path));
}
bool remove_stale(const fs::path& path) {
    const auto owner = inspect(path);
    if (owner && owner_process_alive(*owner))
        return false;
    if (!owner && fresh(path, Millis(300000)))
        return false;
    return remove_file(path) || !fs::exists(path);
}
enum class CreateResult { created, exists, busy };
CreateResult create_new_json(const fs::path& path, const Json& value) {
    const auto bytes = value.dump() + '\n';
#ifdef _WIN32
    NativeHandle file(CreateFileW(path.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            return CreateResult::exists;
        if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
            return CreateResult::busy;
        throw Error(windows_error(error));
    }
#else
    NativeHandle file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!file) {
        if (errno == EEXIST)
            return CreateResult::exists;
        throw std::system_error(errno, std::generic_category());
    }
#endif
    ScopeExit cleanup([&] {
        file.reset();
        remove_file(path);
    });
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#ifdef _WIN32
        DWORD count = 0;
        if (!WriteFile(file.get(), bytes.data() + offset,
                       static_cast<DWORD>(std::min<std::size_t>(65536, bytes.size() - offset)), &count,
                       nullptr))
            throw Error(windows_error());
#else
        const auto count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::system_error(errno, std::generic_category());
        }
#endif
        if (!count)
            throw Error("Cannot write scheduler owner file");
        offset += static_cast<std::size_t>(count);
    }
    cleanup.disarm();
    return CreateResult::created;
}
struct Claim {
    OwnedFile owned;
    bool released = false;
    explicit Claim(OwnedFile value) : owned(std::move(value)) {}
    ~Claim() {
        if (!released) {
            try {
                release_owned(owned);
            } catch (...) {
            }
        }
    }
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
    void release() {
        release_owned(owned);
        released = true;
    }
};
std::unique_ptr<Claim> claim_once(const fs::path& path, Json owner) {
    const auto token = uuid();
    owner["token"] = token;
    owner["pid"] = process_id();
    owner["processInstance"] = instance_json();
    owner["acquiredAtUtc"] = utc_now();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = create_new_json(path, owner);
        if (result == CreateResult::created)
            return std::make_unique<Claim>(OwnedFile{path, token});
        if (result != CreateResult::exists || !remove_stale(path))
            break;
    }
    return {};
}
void replace_text(const fs::path& path, std::string_view text) {
    const auto temporary =
        path.parent_path() / path_from_utf8("." + path_text(path.filename()) + "." + uuid() + ".tmp");
    ScopeExit cleanup([&] {
        std::error_code ec;
        fs::remove(temporary, ec);
    });
    write_file(temporary, text);
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error(windows_error());
#else
    fs::rename(temporary, path);
#endif
    cleanup.disarm();
}
std::string increment_sequence(std::string current) {
    const auto seed = std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
    const std::string maximum = "340282366920938463463374607431768211455";
    const auto greater = [](std::string_view a, std::string_view b) {
        return a.size() != b.size() ? a.size() > b.size() : a > b;
    };
    current = trim(current);
    if (current.empty() || current.size() > maximum.size() ||
        !std::all_of(current.begin(), current.end(), [](char c) { return c >= '0' && c <= '9'; }))
        current = "0";
    const auto nonzero = current.find_first_not_of('0');
    current = nonzero == std::string::npos ? "0" : current.substr(nonzero);
    if (greater(current, maximum))
        current = "0";
    if (greater(seed, current))
        current = seed;
    if (current == maximum)
        return current;
    for (auto i = current.size(); i-- > 0;) {
        if (current[i] != '9') {
            ++current[i];
            return current;
        }
        current[i] = '0';
    }
    return "1" + current;
}
struct Plan {
    std::string pool, queue_class;
    std::size_t total, reserved, usable, weight, protected_low = 0;
    bool pressure = false;
    Plan(const SchedulerConfig& config, const AcquireRequest& request, bool constrained) {
        pressure = constrained;
        pool = request.kind == ExecutionKind::background && request.resource_class == ResourceClass::watch
                   ? "watch"
                   : "execution";
        total = pool == "watch" ? config.watch_max_concurrent : config.max_concurrent;
        reserved = pool == "execution" ? std::min(config.reserved_interactive, total - 1) : 0;
        usable = request.kind == ExecutionKind::background && pool == "execution"
                     ? std::max<std::size_t>(total - reserved, 1)
                     : total;
        if (pool == "execution" && (request.resource_class == ResourceClass::heavy ||
                                    request.resource_class == ResourceClass::io_heavy)) {
            auto capacity = request.resource_class == ResourceClass::heavy ? config.heavy_capacity
                                                                           : config.io_heavy_capacity;
            if (pressure)
                capacity = std::min(capacity, std::max<std::size_t>(1, request.weight));
            usable = std::max<std::size_t>(1, std::min(usable, capacity));
        }
        weight = pool == "watch" ? 1 : std::clamp<std::size_t>(request.weight, 1, usable);
        if (pool == "execution" && pressure && request.resource_class == ResourceClass::light)
            protected_low = std::min(std::max(config.heavy_weight, config.io_heavy_weight), usable - 1);
        if (pool == "watch")
            queue_class = "watch";
        else if (request.kind == ExecutionKind::background)
            queue_class = "execution-background";
        else if (pressure)
            queue_class = request.resource_class == ResourceClass::heavy ||
                                  request.resource_class == ResourceClass::io_heavy
                              ? "execution-interactive-weighted"
                              : "execution-interactive-light";
        else
            queue_class = "execution-interactive";
    }
};
bool disk_pressure(const fs::path& root) {
    const auto path = root / ".disk-pressure.json";
    if (!fresh(path, Millis(180000)))
        return false;
    const auto value = inspect(path);
    const auto pressure = value ? json_string(*value, "diskPressure") : "";
    return pressure == "warning" || pressure == "critical";
}
bool reap_ticket(std::string_view name, std::string_view prefix, const Json& owner) {
    if (!owner_process_alive(owner))
        return true;
    if (owner.contains("processInstance") && !owner["processInstance"].is_null())
        return false;
    auto queued = json_uint(owner, "queuedAtUnixMs");
    if (!queued && name.starts_with(prefix)) {
        auto sequence = std::string(name.substr(prefix.size()));
        sequence = sequence.substr(0, sequence.find('-'));
        if (sequence.size() > 6) {
            sequence.resize(sequence.size() - 6);
            try {
                queued = std::stoull(sequence);
            } catch (...) {
            }
        }
    }
    const auto timeout = json_uint(owner, "queueTimeoutMs", 300000);
    return queued && unix_millis() > queued && unix_millis() - queued > timeout + 1000;
}
bool valid_ticket_name(std::string_view name, std::string_view class_name) {
    return name.starts_with(std::string(class_name) + '-') && name.ends_with(".json") &&
           name.find_first_of("/\\") == std::string_view::npos;
}
std::optional<std::string> refresh_head_locked(const fs::path& root, std::string_view class_name) {
    std::optional<std::string> head;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = path_text(entry.path().filename());
        if (valid_ticket_name(name, class_name) && (!head || name < *head))
            head = name;
    }
    const auto head_path = root / path_from_utf8("." + std::string(class_name) + "-head.json");
    if (head)
        replace_text(head_path, Json{{"name", *head}}.dump());
    else
        remove_file(head_path);
    return head;
}
std::unique_ptr<Claim> head_claim(const fs::path& root, std::string_view class_name) {
    return claim_once(root / path_from_utf8("." + std::string(class_name) + "-head.lock"),
                      Json{{"class", class_name}});
}
bool aged_background_competes(const SchedulerConfig& config, const Plan& interactive) {
    const auto root = config.root / "queue";
    if (!fs::exists(root))
        return false;
    const auto now = unix_millis();
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = path_text(entry.path().filename());
        if (!valid_ticket_name(name, "execution-background"))
            continue;
        const auto owner = inspect(entry.path());
        if (!owner || reap_ticket(name, "execution-background-", *owner))
            continue;
        const auto queued = json_uint(*owner, "queuedAtUnixMs", now);
        if (now < queued || now - queued < static_cast<std::uint64_t>(config.background_priority_age.count()))
            continue;
        AcquireRequest request{ExecutionKind::background,
                               resource_class(json_string(*owner, "resourceClass", "light")),
                               static_cast<std::size_t>(json_uint(*owner, "weight", 1)),
                               "aged-background-probe",
                               {}};
        const Plan background(config, request, interactive.pressure);
        if (background.pool == interactive.pool && interactive.protected_low < background.usable &&
            background.protected_low < interactive.usable)
            return true;
    }
    return false;
}
fs::path slot_path(const fs::path& root, std::string_view pool, std::size_t index) {
    std::ostringstream name;
    name << (pool == "watch" ? "watch-slot-" : "slot-") << std::setfill('0') << std::setw(2) << index
         << ".json";
    return root / path_from_utf8(name.str());
}
} // namespace
struct ExecutionLease::State {
    std::vector<OwnedFile> owned;
    std::shared_ptr<SchedulerMetrics> metrics;
    std::string class_name;
    bool released = false;
    ~State() {
        if (!released) {
            try {
                release();
            } catch (...) {
                mark_released();
            }
        }
    }
    void mark_released() {
        if (released)
            return;
        released = true;
        std::lock_guard lock(metrics->mutex);
        if (metrics->active)
            --metrics->active;
        auto& metric = metrics->classes[class_name];
        if (metric.active)
            --metric.active;
    }
    void release() {
        if (released)
            return;
        std::string failures;
        for (const auto& file : owned) {
            try {
                release_owned(file);
            } catch (const std::exception& error) {
                failures += std::string(error.what()) + "; ";
            }
        }
        if (!failures.empty())
            throw Error(failures);
        mark_released();
    }
};
ExecutionLease::ExecutionLease() = default;
ExecutionLease::~ExecutionLease() = default;
ExecutionLease::ExecutionLease(ExecutionLease&&) noexcept = default;
ExecutionLease& ExecutionLease::operator=(ExecutionLease&&) noexcept = default;
void ExecutionLease::release() {
    if (state_)
        state_->release();
}
Json ExecutionLease::json() const {
    return Json{{"slot", slots.empty() ? Json(nullptr) : Json(slots.front())},
                {"slots", slots},
                {"kind", execution_name(kind)},
                {"pool", pool},
                {"resourceClass", resource_name(resource_class)},
                {"weight", weight},
                {"queueWaitMs", queue_wait_ms}};
}
struct ExecutionWaiter::State {
    SchedulerConfig config;
    std::shared_ptr<SchedulerMetrics> metrics;
    AcquireRequest request;
    Clock::time_point started;
    Millis timeout;
    Plan plan;
    std::optional<OwnedFile> ticket;
    bool pending = true;
    bool finished = false;
    State(SchedulerConfig cfg, std::shared_ptr<SchedulerMetrics> met, AcquireRequest req)
        : config(std::move(cfg)), metrics(std::move(met)), request(std::move(req)), started(Clock::now()),
          timeout(std::max(request.queue_timeout.value_or(config.queue_timeout), Millis(1))),
          plan(config, request, disk_pressure(config.root)) {
        std::lock_guard lock(metrics->mutex);
        ++metrics->queued;
    }
    ~State() {
        try {
            release_ticket();
        } catch (...) {
        }
        end_wait(false, !finished);
    }
    void end_wait(bool timed_out, bool cancelled) {
        if (!pending)
            return;
        pending = false;
        std::lock_guard lock(metrics->mutex);
        if (metrics->queued)
            --metrics->queued;
        if (timed_out)
            ++metrics->timed_out;
        if (cancelled)
            ++metrics->cancelled;
    }
    void release_ticket() {
        if (!ticket)
            return;
        release_owned(*ticket);
        ticket.reset();
        const auto root = config.root / "queue";
        if (auto claim = head_claim(root, plan.queue_class)) {
            refresh_head_locked(root, plan.queue_class);
            claim->release();
        }
    }
    QueueTimeout timeout_error(bool weighted = false) const {
        const auto elapsed = elapsed_ms(started);
        const auto message = "Execution queue remained saturated for " + std::to_string(elapsed) +
                             (weighted ? " ms while reserving weighted capacity."
                                       : " ms. Retry shortly or use a detached job for long work.");
        return QueueTimeout(message, Json{{"kind", execution_name(request.kind)},
                                          {"label", request.label},
                                          {"pool", plan.pool},
                                          {"resource_class", resource_name(request.resource_class)},
                                          {"weight", plan.weight},
                                          {"queue_wait_ms", elapsed},
                                          {"max_concurrent", plan.total},
                                          {"reserved_interactive", plan.reserved}});
    }
    bool create_ticket() {
        const auto root = config.root / "queue";
        fs::create_directories(root);
        auto claim = head_claim(root, plan.queue_class);
        if (!claim)
            return false;
        const auto sequence_path = root / path_from_utf8("." + plan.queue_class + "-sequence.txt");
        std::string current;
        try {
            current = read_file(sequence_path, 128);
        } catch (...) {
        }
        const auto sequence = increment_sequence(current);
        replace_text(sequence_path, sequence + '\n');
        const auto padded = std::string(sequence.size() < 32 ? 32 - sequence.size() : 0, '0') + sequence;
        const auto token = uuid();
        const auto name = plan.queue_class + '-' + padded + '-' + token + ".json";
        const auto path = root / path_from_utf8(name);
        const auto owner = Json{{"token", token},
                                {"pid", process_id()},
                                {"processInstance", instance_json()},
                                {"class", plan.queue_class},
                                {"kind", execution_name(request.kind)},
                                {"resourceClass", resource_name(request.resource_class)},
                                {"weight", request.weight},
                                {"label", request.label},
                                {"sequence", sequence},
                                {"queuedAtUnixMs", unix_millis()},
                                {"queuedAtUtc", utc_now()},
                                {"queueTimeoutMs", timeout.count()}};
        replace_text(path, owner.dump() + '\n');
        ticket = OwnedFile{path, token};
        refresh_head_locked(root, plan.queue_class);
        claim->release();
        return true;
    }
    bool is_head() {
        const auto root = config.root / "queue";
        const auto head_path = root / path_from_utf8("." + plan.queue_class + "-head.json");
        const auto head = inspect(head_path);
        auto name = head ? json_string(*head, "name") : "";
        const auto refresh = [&] {
            if (auto claim = head_claim(root, plan.queue_class)) {
                refresh_head_locked(root, plan.queue_class);
                claim->release();
            }
        };
        if (!valid_ticket_name(name, plan.queue_class)) {
            refresh();
            return false;
        }
        if (name == path_text(ticket->path.filename()))
            return true;
        const auto path = root / path_from_utf8(name);
        const auto owner = inspect(path);
        if (!owner) {
            if (fresh(path, Millis(5000)))
                return false;
            remove_file(path);
            refresh();
            return false;
        }
        if (reap_ticket(name, plan.queue_class + '-', *owner)) {
            remove_file(path);
            refresh();
        }
        return false;
    }
    std::optional<std::vector<OwnedFile>> claim_slots() {
        std::unique_ptr<Claim> weighted;
        if (plan.weight > 1) {
            weighted = claim_once(config.root / path_from_utf8(plan.pool + "-weighted-claim.json"),
                                  Json{{"pool", plan.pool}});
            if (!weighted)
                return std::nullopt;
        }
        std::vector<OwnedFile> owned;
        ScopeExit cleanup([&] {
            for (const auto& value : owned) {
                try {
                    release_owned(value);
                } catch (...) {
                }
            }
        });
        for (auto index = plan.protected_low; index < plan.usable && owned.size() < plan.weight; ++index) {
            const auto path = slot_path(config.root, plan.pool, index);
            const auto token = uuid();
            const auto owner = Json{{"token", token},
                                    {"pid", process_id()},
                                    {"processInstance", instance_json()},
                                    {"kind", execution_name(request.kind)},
                                    {"pool", plan.pool},
                                    {"resourceClass", resource_name(request.resource_class)},
                                    {"weight", plan.weight},
                                    {"label", request.label},
                                    {"acquiredAtUtc", utc_now()}};
            auto result = create_new_json(path, owner);
            if (result == CreateResult::exists && remove_stale(path))
                result = create_new_json(path, owner);
            if (result == CreateResult::created)
                owned.push_back({path, token, index});
        }
        if (weighted)
            weighted->release();
        if (owned.size() != plan.weight)
            return std::nullopt;
        cleanup.disarm();
        return owned;
    }
};
ExecutionWaiter::ExecutionWaiter(std::unique_ptr<State> state) : state_(std::move(state)) {}
ExecutionWaiter::~ExecutionWaiter() = default;
ExecutionWaiter::ExecutionWaiter(ExecutionWaiter&&) noexcept = default;
ExecutionWaiter& ExecutionWaiter::operator=(ExecutionWaiter&&) noexcept = default;
Millis ExecutionWaiter::poll_interval() const {
    const auto elapsed = Clock::now() - state_->started;
    const auto interval = elapsed < Millis(1000)    ? Millis(50)
                          : elapsed < Millis(5000)  ? Millis(100)
                          : elapsed < Millis(30000) ? Millis(250)
                                                    : Millis(500);
    return std::max(Millis(1),
                    std::min(interval, state_->timeout - std::chrono::duration_cast<Millis>(elapsed)));
}
std::optional<ExecutionLease> ExecutionWaiter::poll(const Cancel& cancel) {
    auto& state = *state_;
    if (state.finished)
        throw Error("Execution waiter has already finished");
    try {
        if (cancel && cancel->cancelled())
            throw QueueCancelled();
        if (Clock::now() - state.started >= state.timeout)
            throw state.timeout_error();
        if (!state.ticket && !state.create_ticket())
            return std::nullopt;
        if (!state.is_head())
            return std::nullopt;
        if (state.request.kind == ExecutionKind::interactive &&
            aged_background_competes(state.config, state.plan))
            return std::nullopt;
        auto owned = state.claim_slots();
        if (!owned)
            return std::nullopt;
        ScopeExit cleanup([&] {
            for (const auto& file : *owned) {
                try {
                    release_owned(file);
                } catch (...) {
                }
            }
        });
        state.release_ticket();
        ExecutionLease lease;
        lease.kind = state.request.kind;
        lease.resource_class = state.request.resource_class;
        lease.pool = state.plan.pool;
        lease.weight = state.plan.weight;
        lease.queue_wait_ms = elapsed_ms(state.started);
        for (const auto& file : *owned)
            lease.slots.push_back(file.index);
        lease.state_ = std::make_unique<ExecutionLease::State>();
        lease.state_->owned = *owned;
        lease.state_->metrics = state.metrics;
        lease.state_->class_name = resource_name(lease.resource_class);
        {
            auto& metrics = *state.metrics;
            std::lock_guard lock(metrics.mutex);
            ++metrics.active;
            ++metrics.acquired;
            metrics.total_wait += lease.queue_wait_ms;
            metrics.max_wait = std::max(metrics.max_wait, lease.queue_wait_ms);
            auto& metric = metrics.classes[lease.state_->class_name];
            ++metric.active;
            ++metric.acquired;
            metric.total_wait += lease.queue_wait_ms;
            metric.max_wait = std::max(metric.max_wait, lease.queue_wait_ms);
        }
        state.finished = true;
        state.end_wait(false, false);
        cleanup.disarm();
        return lease;
    } catch (const QueueTimeout&) {
        state.finished = true;
        state.end_wait(true, false);
        throw;
    } catch (const QueueCancelled&) {
        state.finished = true;
        state.end_wait(false, true);
        throw;
    } catch (...) {
        state.finished = true;
        state.end_wait(false, false);
        throw;
    }
}
ExecutionScheduler::ExecutionScheduler(SchedulerConfig config)
    : config_(config.normalized()), metrics_(std::make_shared<SchedulerMetrics>()) {}
ExecutionWaiter ExecutionScheduler::begin(AcquireRequest request) const {
    fs::create_directories(config_.root);
    return ExecutionWaiter(std::make_unique<ExecutionWaiter::State>(config_, metrics_, std::move(request)));
}
ExecutionLease ExecutionScheduler::acquire(AcquireRequest request, const Cancel& cancel) const {
    auto waiter = begin(std::move(request));
    while (true) {
        if (auto lease = waiter.poll(cancel))
            return std::move(*lease);
        if (cancel)
            cancel->wait_for(waiter.poll_interval());
        else
            std::this_thread::sleep_for(waiter.poll_interval());
    }
}
Json ExecutionScheduler::snapshot() const {
    fs::create_directories(config_.root);
    const auto pool_entries = [&](std::string_view pool) {
        const std::string prefix = pool == "watch" ? "watch-slot-" : "slot-";
        std::vector<std::pair<std::size_t, Json>> entries;
        for (const auto& entry : fs::directory_iterator(config_.root)) {
            const auto name = path_text(entry.path().filename());
            if (!name.starts_with(prefix) || !name.ends_with(".json"))
                continue;
            const auto index_text = name.substr(prefix.size(), name.size() - prefix.size() - 5);
            if (index_text.empty() || !std::all_of(index_text.begin(), index_text.end(),
                                                   [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            std::size_t index;
            try {
                index = std::stoull(index_text);
            } catch (...) {
                continue;
            }
            auto owner = inspect(entry.path());
            if (!owner)
                continue;
            if (!owner_process_alive(*owner)) {
                remove_file(entry.path());
                continue;
            }
            (*owner)["slot"] = index;
            entries.emplace_back(index, *owner);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        Json values = Json::array();
        for (auto& entry : entries)
            values.push_back(std::move(entry.second));
        return values;
    };
    const auto occupied = pool_entries("execution"), watch = pool_entries("watch");
    std::map<std::string, std::size_t> queues;
    const auto queue_root = config_.root / "queue";
    if (fs::exists(queue_root))
        for (const auto& entry : fs::directory_iterator(queue_root)) {
            const auto name = path_text(entry.path().filename());
            if (!name.ends_with(".json"))
                continue;
            const auto owner = inspect(entry.path());
            if (!owner)
                continue;
            const auto class_name = json_string(*owner, "class");
            if (class_name != "watch" && !class_name.starts_with("execution-interactive") &&
                !class_name.starts_with("execution-background"))
                continue;
            if (reap_ticket(name, class_name + '-', *owner))
                remove_file(entry.path());
            else
                ++queues[class_name];
        }
    std::size_t total = 0;
    Json by_class = Json::object();
    for (const auto& [name, count] : queues) {
        total += count;
        by_class[name] = count;
    }
    return Json{{"max_concurrent", config_.max_concurrent},
                {"reserved_interactive", config_.reserved_interactive},
                {"heavy_capacity", config_.heavy_capacity},
                {"io_heavy_capacity", config_.io_heavy_capacity},
                {"background_priority_age_ms", config_.background_priority_age.count()},
                {"background_capacity",
                 std::max<std::size_t>(1, config_.max_concurrent - config_.reserved_interactive)},
                {"watch_capacity", config_.watch_max_concurrent},
                {"occupied", occupied.size()},
                {"occupied_slots", occupied},
                {"watch_occupied", watch.size()},
                {"watch_slots", watch},
                {"global_queued", total},
                {"global_queued_by_class", by_class},
                {"local_process", metrics_->snapshot()}};
}
} // namespace devbox
