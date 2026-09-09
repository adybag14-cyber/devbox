#include "devbox/telemetry.hpp"
#include <algorithm>
#include <fstream>
#include <set>
namespace devbox {
namespace {
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
std::string preview(std::string_view value, std::size_t units, bool ellipsis = true) {
    if (js_length(value) <= units)
        return std::string(value);
    auto utf = to_utf16(value);
    if (units && utf[units - 1] >= 0xd800 && utf[units - 1] <= 0xdbff)
        --units;
    return from_utf16(std::u16string_view(utf).substr(0, units)) + (ellipsis ? "..." : "");
}
bool sensitive(const std::string& name) {
    const auto key = lower(name);
    for (auto word :
         {"token", "secret", "password", "authorization", "cookie", "content_base64", "expected_sha256"})
        if (key.find(word) != key.npos)
            return true;
    return false;
}
Json summarize(const std::string& key, const Json& value, unsigned depth, std::size_t& budget) {
    if (!budget || depth > 16)
        return Json{{"omitted", true}};
    --budget;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        Json result{{"type", "string"}, {"length", js_length(text)}};
        if (sensitive(key))
            result["redacted"] = true;
        else
            result["preview"] = preview(text, 240);
        return result;
    }
    if (value.is_array()) {
        Json samples = Json::array();
        for (std::size_t i = 0; i < std::min<std::size_t>(8, value.size()) && budget; ++i)
            samples.push_back(summarize(key + "[]", value[i], depth + 1, budget));
        return Json{{"type", "array"}, {"length", value.size()}, {"sample", samples}};
    }
    if (value.is_object()) {
        Json result = Json::object();
        auto sorted = canonical_json(value);
        std::size_t count = 0;
        for (auto it = sorted.begin(); it != sorted.end() && count < 12 && budget; ++it, ++count)
            result[it.key()] = summarize(key + "." + it.key(), it.value(), depth + 1, budget);
        return result;
    }
    return value;
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
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || queue_.size() >= 1024) {
            ++dropped_;
            return;
        }
        queue_.push_back(std::move(event));
        ++enqueued_;
    }
    wake_.notify_one();
}
void UsageLogger::run() {
    for (;;) {
        std::vector<Json> events;
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
                events.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
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
                {"queuedEvents", queue_.size()}};
}
Json summarize_arguments(const Json& arguments) {
    static const std::set<std::string> internal{"signal",
                                                "sessionId",
                                                "_meta",
                                                "authInfo",
                                                "requestId",
                                                "requestInfo",
                                                "taskId",
                                                "taskStore",
                                                "taskRequestedTtl",
                                                "closeSSEStream",
                                                "closeStandaloneSSEStream"};
    Json result = Json::object();
    if (!arguments.is_object())
        return result;
    auto sorted = canonical_json(arguments);
    std::size_t budget = 256;
    for (auto it = sorted.begin(); it != sorted.end() && budget; ++it)
        if (!internal.contains(it.key()))
            result[it.key()] = summarize(it.key(), it.value(), 0, budget);
    return result;
}
UsageTelemetry::UsageTelemetry(const Config& config, BackgroundTasks& background)
    : tools_(config.project_root / "run" / "tool-usage.jsonl", config.usage_log_max_bytes,
             config.usage_log_rotations, background, "usage-tool-writer"),
      http_(config.project_root / "run" / "http-usage.jsonl", config.usage_log_max_bytes,
            config.usage_log_rotations, background, "usage-http-writer") {}
Json UsageTelemetry::Invocation::event(std::string type) const {
    return Json{{"type", type},           {"invocation_id", id}, {"tool", tool}, {"started_at", started_at},
                {"arguments", arguments}, {"context", context}};
}
std::string UsageTelemetry::started(const std::string& tool, const Json& args, const Json& context) {
    Invocation invocation{uuid(), tool, utc_now(), Clock::now(), summarize_arguments(args), context};
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
    event.update(Json{{"finished_at", utc_now()},
                      {"duration_ms", elapsed(inv->start)},
                      {"ok", json_bool(structured, "ok", !json_bool(response, "isError"))},
                      {"is_error", json_bool(response, "isError")},
                      {"summary", preview(summary, 4096, false)},
                      {"summary_truncated", js_length(summary) > 4096},
                      {"result_text_chars", result_chars},
                      {"stdout_chars", js_length(text_member(structured, "stdout"))},
                      {"stderr_chars", js_length(text_member(structured, "stderr"))},
                      {"exit_code", member(structured, "exitCode")},
                      {"truncated", json_bool(structured, "truncated")},
                      {"queue_wait_ms", member(execution, "queue_wait_ms")},
                      {"execution_slot", member(execution, "slot")}});
    tools_.enqueue(std::move(event));
}
void UsageTelemetry::failed(const std::string& id, const std::string& error) {
    auto inv = remove(id);
    if (!inv)
        return;
    auto event = inv->event("tool_throw");
    event.update(Json{{"finished_at", utc_now()},
                      {"duration_ms", elapsed(inv->start)},
                      {"error", preview(error.empty() ? "The command failed." : error, 4096, false)},
                      {"error_truncated", js_length(error) > 4096}});
    tools_.enqueue(std::move(event));
}
void UsageTelemetry::http(const HttpRequest& request, int status, Millis duration, bool disconnected) {
    auto forwarded = json_string(request.headers, "x-forwarded-for");
    forwarded = trim(forwarded.substr(0, forwarded.find(',')));
    http_.enqueue(Json{{"type", "http_request"},
                       {"request_id", request.usage_id},
                       {"started_at", request.started_at},
                       {"finished_at", utc_now()},
                       {"duration_ms", duration.count()},
                       {"method", request.method},
                       {"path", request.path},
                       {"status_code", disconnected || !status ? Json() : Json(status)},
                       {"outcome", disconnected ? "client_aborted" : "finished"},
                       {"client_aborted", disconnected},
                       {"accept", json_string(request.headers, "accept")},
                       {"user_agent", json_string(request.headers, "user-agent")},
                       {"forwarded_for", forwarded.empty() ? Json() : Json(forwarded)}});
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
Json UsageTelemetry::snapshot() const {
    return Json{{"tool", tools_.snapshot()}, {"http", http_.snapshot()}};
}
void UsageTelemetry::stop() {
    tools_.stop();
    http_.stop();
}
} // namespace devbox
