#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include "devbox/result.hpp"
#include "devbox/telemetry.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
namespace devbox {
namespace {
std::uint64_t elapsed_us(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Micros>(Clock::now() - start).count());
}
Json trace_build() {
    const auto build = build_snapshot();
    Json result = Json::object();
    for (const auto* key : {"gitSha", "sourceTree", "binarySha256", "deploymentGeneration"})
        result[key] = build[key];
#ifdef _WIN32
    result["pid"] = GetCurrentProcessId();
#else
    result["pid"] = getpid();
#endif
    return result;
}
std::string tool_outcome(const Json& structured, const Json& response, std::string_view tool) {
    if (const auto classified = result_outcome(response); !classified.empty())
        return classified;
    const auto code = json_string(structured, "error_code");
    if (code.find("CANCEL") != code.npos)
        return "cancelled";
    if (code.find("TIMEOUT") != code.npos || code.find("TIMED_OUT") != code.npos)
        return "timed_out";
    if (code.find("DENIED") != code.npos || code.find("POLICY") != code.npos ||
        code.find("SCOPE") != code.npos)
        return "policy_denied";
    const auto exit = structured.find("exitCode");
    if (exit != structured.end() && exit->is_number_integer() && exit->get<std::int64_t>() != 0)
        return "application_exit";
    if (!json_bool(structured, "ok", !json_bool(response, "isError")))
        return "tool_error"; // An unclassified tool failure is not asserted to be infrastructure failure.
    return tool == "devbox_wait" || tool == "devbox_job_status" ? "wait_completed" : "success";
}
std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Millis>(Clock::now() - start).count());
}
const Json& member(const Json& value, std::string_view key) {
    static const Json absent;
    const auto found = value.find(key);
    return found == value.end() ? absent : *found;
}
std::string_view text_member(const Json& value, std::string_view key) {
    const auto& field = member(value, key);
    return field.is_string() ? std::string_view(field.get_ref<const std::string&>()) : std::string_view();
}
Json redacted_shape(const Json& value) {
    Json result{{"type", value.type_name()}, {"redacted", true}};
    if (value.is_string())
        result["length"] = js_length(value.get_ref<const std::string&>());
    else if (value.is_structured())
        result["length"] = value.size();
    return result;
}
// A conservative allocation/encoding charge; stop walking oversized or adversarial trees.
std::size_t event_charge(const Json& value, std::size_t limit, unsigned depth = 0) {
    if (depth > 32 || limit < 128)
        return limit + 1;
    std::size_t size = 128;
    if (value.is_binary()) {
        const auto bytes = value.get_binary().size();
        return bytes > (limit - size) / 6 ? limit + 1 : size + bytes * 6;
    }
    if (value.is_string()) {
        const auto chars = value.get_ref<const std::string&>().size();
        return chars > (limit - size) / 6 ? limit + 1 : size + chars * 6;
    }
    if (value.is_structured())
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (value.is_object()) {
                if (it.key().size() > (limit - size) / 6)
                    return limit + 1;
                size += it.key().size() * 6;
            }
            const auto child = event_charge(*it, limit - size, depth + 1);
            if (child > limit - size)
                return limit + 1;
            size += child;
        }
    return size;
}
} // namespace
JsonLogSink::JsonLogSink(fs::path path, std::uint64_t maximum, std::size_t rotations)
    : path_(std::move(path)), maximum_(maximum), rotations_(rotations) {}
void JsonLogSink::append(const Json& event) {
    append_batch({&event, 1});
}
void JsonLogSink::append_batch(std::span<const Json> events) {
    if (events.empty())
        return;
    ensure_directory(path_.parent_path());
    std::error_code ec;
    auto size = fs::file_size(path_, ec);
    if (ec)
        size = 0;
    std::ofstream file;
    auto close = [&] {
        if (!file.is_open())
            return;
        file.close();
        if (!file)
            throw Error("Cannot close " + path_text(path_));
    };
    auto rotate = [&] {
        close();
        auto rotation = [this](std::size_t i) {
            auto p = path_;
            p += "." + std::to_string(i);
            return p;
        };
        fs::remove(rotation(rotations_), ec);
        if (ec)
            throw Error("remove rotated usage log: " + ec.message());
        for (auto i = rotations_; i > 1; --i)
            if (fs::exists(rotation(i - 1)))
                replace_state_file(rotation(i - 1), rotation(i));
        if (fs::exists(path_))
            replace_state_file(path_, rotation(1));
        size = 0;
    };
    std::string pending;
    pending.reserve(std::min<std::size_t>(events.size() * 512, 65536));
    auto flush = [&] {
        if (pending.empty())
            return;
        // Keep one stream for the batch, including when its records need
        // several bounded writes. Release it before rotation and on return,
        // so a later batch observes external file replacement as before.
        if (!file.is_open()) {
            file.open(path_, std::ios::binary | std::ios::app);
            if (!file)
                throw Error("Cannot open " + path_text(path_));
        }
        file.write(pending.data(), static_cast<std::streamsize>(pending.size()));
        file.flush();
        if (!file)
            throw Error("Cannot write " + path_text(path_));
        size += pending.size();
        pending.clear();
    };
    for (const auto& event : events) {
        auto bytes = json_dump(event, Json::error_handler_t::replace) + '\n';
        const auto buffered_size = size + pending.size();
        if (maximum_ && rotations_ &&
            (buffered_size >= maximum_ || bytes.size() >= maximum_ - buffered_size)) {
            flush();
            rotate();
        }
        pending += bytes;
        if (pending.size() >= 65536)
            flush();
    }
    flush();
    close();
}
UsageLogger::UsageLogger(fs::path path, std::uint64_t maximum, std::size_t rotations,
                         BackgroundTasks& background, std::string name)
    : sink_(std::move(path), maximum, rotations), background_(background), name_(std::move(name)) {
    background_.mark_started(name_);
    thread_ = std::thread([this] { run(); });
}
UsageLogger::~UsageLogger() {
    stop();
}
void UsageLogger::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
void UsageLogger::enqueue(Json event) {
    const auto bytes = event_charge(event, max_event_bytes_);
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= 1024 || bytes > max_event_bytes_ ||
            bytes > capacity_bytes_ - resident_bytes_) {
            ++dropped_;
            return;
        }
        queue_.emplace_back(std::move(event), bytes);
        resident_bytes_ += bytes;
        ++enqueued_;
    }
    wake_.notify_one();
}
void UsageLogger::run() {
    for (;;) {
        std::vector<Json> events;
        std::size_t batch_bytes = 0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
                break;
            if (queue_.size() < 64 && !stopping_)
                wake_.wait_for(lock, Millis(2), [this] { return stopping_ || queue_.size() >= 64; });
            // Drain more of an existing backlog per filesystem metadata lookup.
            // The queue bound, coalescing deadline and checked write flushes
            // remain unchanged; producers regain space before the batch is sent.
            const auto count = std::min<std::size_t>(256, queue_.size());
            events.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                events.push_back(std::move(queue_.front().first));
                batch_bytes += queue_.front().second;
                queue_.pop_front();
            }
        }
        ScopeExit release_budget([&] {
            std::lock_guard lock(mutex_);
            resident_bytes_ -= batch_bytes;
        });
        background_.attempt(name_);
        try {
            sink_.append_batch(events);
            background_.success(name_);
        } catch (const std::exception& e) {
            ++failures_;
            background_.failure(name_, e.what());
        }
    }
    background_.mark_stopped(name_);
}
Json UsageLogger::snapshot() const {
    std::lock_guard lock(mutex_);
    return Json{{"enqueued", enqueued_.load()},
                {"dropped", dropped_.load()},
                {"writeFailures", failures_.load()},
                {"capacityEvents", 1024},
                {"capacityBytes", capacity_bytes_},
                {"maxEventBytes", max_event_bytes_},
                {"queuedAndInflightBytes", resident_bytes_},
                {"byteAccounting", "conservative_allocation_and_encoding_charge"},
                {"queuedEvents", queue_.size()}};
}
Json summarize_arguments(const Json& arguments, std::string_view tool) {
    Json result = Json::object();
    if (!arguments.is_object())
        return result;
    const auto& policy = tool_policy(tool);
    std::set<std::string> known, safe;
    auto collect = [&](const Json& entry) {
        for (const auto& [profile, schema] : entry["schemas"].items())
            for (const auto& [key, property] : schema["inputSchema"]["properties"].items())
                known.insert(key);
    };
    if (policy.is_null())
        for (const auto& entry : tool_registry()["tools"])
            collect(entry);
    else {
        collect(policy);
        for (const auto& field : policy["telemetry"]["safe_fields"])
            safe.insert(field.get<std::string>());
    }
    std::size_t unknown = 0;
    for (const auto& [key, value] : arguments.items()) {
        if (key == "signal" || key == "_meta" || key == "authInfo")
            continue;
        if (!known.contains(key)) {
            ++unknown;
            continue;
        }
        bool safe_value = false;
        if (safe.contains(key)) {
            const auto& schemas = policy["schemas"];
            const auto& property = schemas.begin().value()["inputSchema"]["properties"][key];
            if (property.contains("enum"))
                safe_value = std::find(property["enum"].begin(), property["enum"].end(), value) !=
                             property["enum"].end();
            else if (value.is_boolean() && json_string(property, "type") == "boolean")
                safe_value = true;
            else if (value.is_number() && std::isfinite(value.get<double>())) {
                const auto n = value.get<double>();
                safe_value = n >= json_number(property, "minimum", 0) &&
                             n <= json_number(property, "maximum", 86400000);
            }
        }
        result[key] = safe_value ? value : redacted_shape(value);
    }
    if (unknown)
        result["unrecognized_argument_count"] = unknown;
    return result;
}
UsageTelemetry::UsageTelemetry(const Config& config, BackgroundTasks& background)
    : tools_(config.project_root / "run" / "tool-usage.jsonl", config.usage_log_max_bytes,
             config.usage_log_rotations, background, "usage-tool-writer"),
      http_(config.project_root / "run" / "http-usage.jsonl", config.usage_log_max_bytes,
            config.usage_log_rotations, background, "usage-http-writer"),
      build_(trace_build()) {}
Json UsageTelemetry::Invocation::event(std::string type) const {
    Json result{{"type", type},           {"invocation_id", id}, {"tool", tool}, {"started_at", started_at},
                {"arguments", arguments}, {"context", context}};
    if (tool == "host_computer_use" || tool == "host_computer_windows")
        result["usage_type"] = "computer_use";
    if (tool.starts_with("devbox_web_"))
        result["usage_type"] = "web_research";
    return result;
}
std::string UsageTelemetry::started(const std::string& tool, const Json& args, const Json& context) {
    auto summarized = summarize_arguments(args, tool);
    // Context headers and caller-provided request IDs are payload, too.
    Json attributed{{"has_request_id", context.contains("request_id")},
                    {"has_session_id", context.contains("session_id")},
                    {"has_client_id", context.contains("client_id")}};
    // These identities come from the transport and verified OAuth store, never tool arguments.
    // Only the server-issued UUID form is recordable; arbitrary headers/request IDs remain redacted.
    for (const auto* key : {"http_request_id", "client_id"}) {
        const auto& field = member(context, key);
        if (!field.is_string())
            continue;
        const auto& value = field.get_ref<const std::string&>();
        bool uuid_form = value.size() == 36;
        for (std::size_t i = 0; uuid_form && i < value.size(); ++i)
            uuid_form = i == 8 || i == 13 || i == 18 || i == 23
                            ? value[i] == '-'
                            : (value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f');
        if (uuid_form)
            attributed[key] = value;
    }
    attributed["build"] = build_;
    attributed["trace_schema"] = 2;
    const auto positive_wait = [&](std::string_view key) {
        const auto& value = member(args, key);
        return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() > 0;
    };
    attributed["duration_includes_requested_wait"] =
        (tool == "devbox_wait" && positive_wait("seconds")) ||
        ((tool == "devbox_job_status" || tool == "devbox_wait_for_file") &&
         (positive_wait("wait_seconds") || positive_wait("timeout_seconds")));
    const auto start = Clock::now();
    Invocation invocation{uuid(),
                          tool_policy(tool).is_null() ? "unknown_tool" : tool,
                          utc_from_micros(static_cast<std::int64_t>(unix_micros())),
                          start,
                          std::move(summarized),
                          std::move(attributed)};
    const auto id = invocation.id;
    tools_.enqueue(invocation.event("tool_start"));
    std::lock_guard lock(mutex_);
    active_.emplace(id, std::move(invocation));
    return id;
}
std::optional<UsageTelemetry::Invocation> UsageTelemetry::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = active_.find(id);
    if (it == active_.end())
        return {};
    auto value = std::move(it->second);
    active_.erase(it);
    return value;
}
void UsageTelemetry::finished(const std::string& id, const Json& response) {
    auto inv = remove(id);
    if (!inv)
        return;
    auto event = inv->event("tool_finish");
    const auto& structured = member(response, "structuredContent");
    auto summary = json_string(structured, "summary", "Tool completed.");
    if (summary.empty())
        summary = "Tool completed.";
    const auto& data = member(structured, "data");
    const auto& execution = member(data, "execution");
    std::size_t result_chars = 0;
    for (const auto& part : member(response, "content"))
        if (json_string(part, "type") == "text")
            result_chars += js_length(text_member(part, "text"));
    const auto total_us = elapsed_us(inv->start);
    event.update(Json{{"finished_at", utc_from_micros(static_cast<std::int64_t>(unix_micros()))},
                      {"duration_ms", total_us / 1000},
                      {"duration_us", total_us},
                      {"outcome", tool_outcome(structured, response, inv->tool)},
                      {"ok", json_bool(structured, "ok", !json_bool(response, "isError"))},
                      {"is_error", json_bool(response, "isError")},
                      {"summary", "Payload omitted by telemetry policy."},
                      {"summary_chars", js_length(summary)},
                      {"summary_redacted", true},
                      {"result_text_chars", result_chars},
                      {"stdout_chars", js_length(text_member(structured, "stdout"))},
                      {"stderr_chars", js_length(text_member(structured, "stderr"))},
                      {"exit_code", member(structured, "exitCode")},
                      {"truncated", json_bool(structured, "truncated")},
                      {"queue_wait_ms", member(execution, "queue_wait_ms")},
                      {"execution_slot", member(execution, "slot")}});
    if (member(execution, "queue_wait_ms").is_number_unsigned() ||
        member(execution, "queue_wait_ms").is_number_integer()) {
        const auto queue_us = json_uint(execution, "queue_wait_ms") * 1000;
        event["queue_wait_us"] = queue_us;
        event["post_admission_us"] = total_us > queue_us ? total_us - queue_us : 0;
    }
    tools_.enqueue(std::move(event));
}
void UsageTelemetry::failed(const std::string& id, const std::string& error) {
    auto inv = remove(id);
    if (!inv)
        return;
    auto event = inv->event("tool_throw");
    const auto total_us = elapsed_us(inv->start);
    event.update(Json{{"finished_at", utc_from_micros(static_cast<std::int64_t>(unix_micros()))},
                      {"duration_ms", total_us / 1000},
                      {"duration_us", total_us},
                      {"outcome", "handler_exception"},
                      {"error", "Handler exception; payload omitted by telemetry policy."},
                      {"error_chars", js_length(error)},
                      {"error_redacted", true}});
    tools_.enqueue(std::move(event));
}
void UsageTelemetry::http(const HttpRequest& request, int status, Millis duration, bool disconnected) {
    auto forwarded = json_string(request.headers, "x-forwarded-for");
    forwarded = trim(forwarded.substr(0, forwarded.find(',')));
    Json event{
        {"type", "http_request"},
        {"trace_schema", 2},
        {"build", build_},
        {"request_id", request.usage_id},
        {"started_at", request.started_at},
        {"connection_created_at", request.connection_created_at},
        {"receive_started_at", request.receive_started_at},
        {"finished_at", request.finished_at.empty() ? utc_now() : request.finished_at},
        {"duration_ms", duration.count()},
        {"duration_us", request.timing.completed ? request.timing.duration_us
                                                 : static_cast<std::uint64_t>(duration.count()) * 1000},
        {"timing_source", request.timing.completed ? "paired_wall_steady" : "legacy_observer"},
        {"method", std::set<std::string>{"GET", "POST", "HEAD", "OPTIONS", "DELETE", "PUT", "PATCH"}.contains(
                       request.method)
                       ? request.method
                       : "unrecognized"},
        {"path", std::set<std::string>{"/", "/mcp", "/healthz", "/livez", "/readyz", "/metadata",
                                       "/oauth/authorize", "/oauth/token", "/oauth/register", "/oauth/revoke",
                                       "/.well-known/oauth-authorization-server",
                                       "/.well-known/oauth-protected-resource"}
                         .contains(request.path)
                     ? request.path
                     : "unrecognized"},
        {"status_code", disconnected || !status ? Json() : Json(status)},
        {"outcome", disconnected ? "client_aborted" : "finished"},
        {"client_aborted", disconnected},
        {"accept", json_string(request.headers, "accept").find("text/event-stream") != std::string::npos
                       ? "sse"
                       : "other"},
        {"user_agent_chars", json_string(request.headers, "user-agent").size()},
        {"forwarded_for_present", !forwarded.empty()}};
    if (request.timing.completed) {
        const auto& t = request.timing;
        event["wall_steady_delta_us"] = t.wall_steady_delta_us;
        event["clock_adjustment_suspected"] =
            t.wall_steady_delta_us > 100000 || t.wall_steady_delta_us < -100000;
        event["phase_timings_us"] = Json{{"receive", t.receive_us},
                                         {"parse", t.parse_us},
                                         {"response_prepare", t.prepare_us},
                                         {"write", t.write_us}};
        const auto accounted = t.receive_us + t.parse_us + t.prepare_us + t.write_us;
        event["phase_timings_us"]["handler_and_wait"] =
            t.duration_us > accounted ? t.duration_us - accounted : 0;
    }
    http_.enqueue(std::move(event));
}
Json UsageTelemetry::active_tools() const {
    std::lock_guard lock(mutex_);
    std::vector<const Invocation*> items;
    for (const auto& [id, inv] : active_) {
        (void)id;
        items.push_back(&inv);
    }
    std::sort(items.begin(), items.end(), [](auto a, auto b) { return a->start < b->start; });
    Json result = Json::array();
    for (std::size_t i = 0; i < std::min<std::size_t>(32, items.size()); ++i) {
        const auto& inv = *items[i];
        result.push_back(Json{{"invocationId", inv.id},
                              {"tool", inv.tool},
                              {"startedAtUtc", inv.started_at},
                              {"elapsedMs", elapsed(inv.start)},
                              {"arguments", inv.arguments},
                              {"context", inv.context}});
    }
    return result;
}
Json UsageTelemetry::active_counts() const {
    std::lock_guard lock(mutex_);
    const auto computer = std::count_if(active_.begin(), active_.end(), [](const auto& entry) {
        return entry.second.tool == "host_computer_use" || entry.second.tool == "host_computer_windows";
    });
    return Json{{"activeTools", active_.size()}, {"activeComputerUse", computer}};
}
Json UsageTelemetry::snapshot() const {
    return Json{{"tool", tools_.snapshot()}, {"http", http_.snapshot()}};
}
void UsageTelemetry::stop() {
    tools_.stop();
    http_.stop();
}
} // namespace devbox
