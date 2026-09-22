#include "devbox/research.hpp"
#include "devbox/storage.hpp"
#include <algorithm>
#include <set>
#include <thread>

namespace devbox::web {
namespace {
constexpr std::uint64_t minimum_interval_ms = 1000;
constexpr std::uint64_t challenge_cooldown_ms = 15 * 60 * 1000;

std::uint64_t unix_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<Millis>(std::chrono::system_clock::now().time_since_epoch()).count());
}
Millis remaining(Clock::time_point deadline) {
    return std::max(Millis(0), std::chrono::duration_cast<Millis>(deadline - Clock::now()));
}
void pause(Millis duration, const Cancel& cancel) {
    if (cancel) {
        if (cancel->wait_for(duration))
            throw Cancelled();
    } else
        std::this_thread::sleep_for(duration);
}
std::uint64_t retry_after_ms(const Transfer& response) {
    return retry_after_millis(json_string(response.headers, "retry-after")).value_or(0);
}
class ProviderHealth {
    fs::path root_;

    Json read() const {
        auto state = read_json_optional(root_ / "health.json", 16384)
                         .value_or(Json{{"version", 1}, {"providers", Json::object()}});
        if (!state.is_object() || json_uint(state, "version") != 1 || !state.contains("providers") ||
            !state["providers"].is_object() || state["providers"].size() > 4)
            throw Error("Invalid bounded search-provider health state");
        return state;
    }

  public:
    explicit ProviderHealth(fs::path root) : root_(std::move(root)) {
        ensure_directory(root_);
    }
    Json reserve(const std::string& provider, Clock::time_point deadline, const Cancel& cancel) {
        for (;;) {
            if (cancel)
                cancel->check();
            const auto left = remaining(deadline);
            if (left.count() <= 0)
                return Json{{"status", "budget_exhausted"}, {"error", "Discovery deadline reached"}};
            std::uint64_t wait_ms = 0;
            {
                FileLock lock(root_ / ".health.lock", std::min(left, Millis(1000)), cancel, true);
                auto state = read();
                auto& entry = state["providers"][provider];
                if (entry.is_null())
                    entry = Json::object();
                if (!entry.is_object())
                    throw Error("Invalid search-provider health entry");
                const auto now = unix_ms();
                const auto cooldown = json_uint(entry, "cooldown_until_ms");
                if (cooldown > now)
                    return Json{
                        {"status", "cooldown"},
                        {"cause", json_string(entry, "reason", "unavailable")},
                        {"retry_after_ms", cooldown - now},
                        {"error",
                         "Provider skipped after an earlier failure; cooldown is shared across jobs"}};
                const auto next = json_uint(entry, "next_request_ms");
                if (next > now)
                    wait_ms = next - now;
                else {
                    entry["next_request_ms"] = now + minimum_interval_ms;
                    write_json_atomic(root_ / "health.json", state);
                    return Json{{"status", "ready"}};
                }
            }
            // Do not consume the entire discovery budget while another caller has a reservation.
            const auto available = remaining(deadline);
            if (available.count() <= 0 || wait_ms >= static_cast<std::uint64_t>(available.count()))
                return Json{{"status", "budget_exhausted"},
                            {"error", "Discovery pacing exceeds remaining budget"}};
            pause(Millis(static_cast<Millis::rep>(wait_ms)), cancel);
        }
    }
    void failed(const std::string& provider, std::string_view reason, const Transfer& response,
                Clock::time_point deadline, const Cancel& cancel) {
        std::uint64_t delay = reason == "challenge_required" || reason == "access_blocked"
                                  ? challenge_cooldown_ms
                              : reason == "parse_error" ? 300000ULL
                                                        : 60000ULL;
        delay = std::max<std::uint64_t>(delay, retry_after_ms(response));
        FileLock lock(root_ / ".health.lock",
                      std::max(Millis(1), std::min(remaining(deadline), Millis(1000))), cancel, true);
        auto state = read();
        const auto now = unix_ms();
        auto& entry = state["providers"][provider];
        if (entry.is_null())
            entry = Json::object();
        if (!entry.is_object())
            throw Error("Invalid search-provider health entry");
        entry["cooldown_until_ms"] = now + std::min(delay, UINT64_MAX - now);
        entry["reason"] = reason;
        write_json_atomic(root_ / "health.json", state);
    }
};
std::string endpoint(const std::string& provider, const std::string& query, const TransportLimits& limits) {
    if (limits.fixture_loopback_port)
        return "http://127.0.0.1:" + std::to_string(*limits.fixture_loopback_port) + "/search/" + provider +
               "?q=" + url_encode(query);
    return discovery_provider(provider).endpoint(query);
}
bool blocked(std::string_view status) {
    return status == "challenge_required" || status == "access_blocked" || status == "rate_limited";
}
} // namespace

Json parse_search_response(std::string_view provider, const Transfer& response,
                           std::optional<unsigned short> fixture_loopback_port) {
    const auto& adapter = discovery_provider(provider);
    Json result{{"status", "unavailable"}, {"urls", Json::array()}, {"results", 0}};
    result["coverage"] = adapter.coverage;
    result["terms_url"] = adapter.terms_url;
    result["use_notice"] = adapter.use_notice;
    if (response.error == "WEB_BYTE_BUDGET" || response.error == "WEB_DEADLINE") {
        result["status"] = "budget_exhausted";
        result["error"] = response.error;
        return result;
    }
    if (response_requires_challenge(response)) {
        result["status"] = "challenge_required";
        result["error"] = "Search provider requires human verification; automatic requests stopped";
        return result;
    }
    if (!response.error.empty() || response.status != 200) {
        result["status"] = response.status == 429                             ? "rate_limited"
                           : response.status == 401 || response.status == 403 ? "access_blocked"
                                                                              : "unavailable";
        result["error"] = response.error.empty()
                              ? "Search provider returned HTTP " + std::to_string(response.status)
                              : response.error;
        return result;
    }
    std::set<std::string> seen;
    const auto add = [&](const std::string& raw) {
        if (raw.empty() || raw.size() > 2048 || result["urls"].size() >= 100)
            return;
        try {
            const auto fixture = fixture_loopback_port
                                     ? "http://127.0.0.1:" + std::to_string(*fixture_loopback_port) + "/"
                                     : "";
            auto url = !fixture.empty() && raw.starts_with(fixture) ? raw : normalize_url(raw);
            if (seen.insert(url).second)
                result["urls"].push_back(std::move(url));
        } catch (...) {
            result["invalid_results"] = json_uint(result, "invalid_results") + 1;
        }
    };
    try {
        adapter.extract(response, add, result);
        if (json_string(result, "status") == "challenge_required")
            return result;
        result["results"] = result["urls"].size();
        if (result["urls"].empty() && json_uint(result, "invalid_results"))
            throw Error("Search results contained no valid public-web URLs");
        result["status"] = result["urls"].empty() ? "no_results" : "ok";
    } catch (const std::exception& error) {
        result["status"] = "parse_error";
        result["error"] = error.what();
        result["urls"] = Json::array();
        result["results"] = 0;
    }
    return result;
}

Json discover_sources(Transport& transport, const Json& plan, const fs::path& health_root,
                      const TransportLimits& limits, Clock::time_point deadline, const Cancel& cancel) {
    Json result{{"urls", Json::array()}, {"providers", Json::array()}};
    const auto mode = json_string(plan, "discovery", "web");
    const auto queries = mode == "none" ? std::vector<std::string>{} : json_strings(plan, "queries");
    Json summary{{"status", mode == "none" ? "disabled" : "complete"},
                 {"queries_total", queries.size()},
                 {"queries_completed", 0},
                 {"queries_with_results", 0},
                 {"queries_failed", 0},
                 {"queries_unattempted", queries.size()},
                 {"fallback_queries", 0},
                 {"provider_attempts", 0},
                 {"http_attempts", 0},
                 {"failed_query_indexes", Json::array()},
                 {"blocked_provider_attempts", 0}};
    if (mode == "none") {
        result["summary"] = summary;
        return result;
    }
    ProviderHealth health(health_root);
    const auto providers = mode == "web"         ? std::vector<std::string>{"duckduckgo_html", "bing_rss"}
                           : mode == "scholarly" ? std::vector<std::string>{"crossref"}
                                                 : std::vector<std::string>{"mediawiki"};
    const auto domains = json_strings(plan, "domains");
    std::size_t completed = 0, failed = 0, attempted = 0, with_results = 0, fallback = 0;
    bool all_failures_blocked = true, exhausted = false;
    for (std::size_t index = 0;
         index < queries.size() && Clock::now() < deadline && !transport.byte_budget_exhausted(); ++index) {
        ++attempted;
        auto query = queries[index];
        if (domains.size() == 1)
            query += " site:" + domains.front();
        bool conclusive = false, found = false;
        for (std::size_t choice = 0; choice < providers.size(); ++choice) {
            const auto& provider = providers[choice];
            Json report{{"query_index", index}, {"query", query}, {"provider", provider}, {"results", 0}};
            const auto& adapter = discovery_provider(provider);
            report["coverage"] = adapter.coverage;
            report["terms_url"] = adapter.terms_url;
            report["use_notice"] = adapter.use_notice;
            try {
                const auto reservation = health.reserve(provider, deadline, cancel);
                if (json_string(reservation, "status") != "ready") {
                    report.update(reservation);
                    const auto status = json_string(reservation, "status");
                    exhausted = exhausted || status == "budget_exhausted";
                    all_failures_blocked = all_failures_blocked && status == "cooldown" &&
                                           blocked(json_string(reservation, "cause"));
                    result["providers"].push_back(std::move(report));
                    continue;
                }
                const auto url = endpoint(provider, query, limits);
                const auto responses = transport.get({Request{url, Json::object()}}, deadline, cancel);
                const auto& response = responses.front();
                if (cancel)
                    cancel->check();
                auto parsed = parse_search_response(provider, response, limits.fixture_loopback_port);
                report["url"] = url;
                report["http_status"] = response.status;
                report["duration_ms"] = response.duration_ms;
                report["request_attempts"] = response.attempts;
                summary["provider_attempts"] = json_uint(summary, "provider_attempts") + 1;
                summary["http_attempts"] = json_uint(summary, "http_attempts") + response.attempts;
                const auto urls = parsed["urls"];
                parsed.erase("urls");
                report.update(parsed);
                const auto status = json_string(parsed, "status");
                if (status == "ok" || status == "no_results") {
                    conclusive = true;
                    if (status == "ok") {
                        found = true;
                        if (choice > 0)
                            ++fallback;
                        for (const auto& candidate : urls)
                            result["urls"].push_back(candidate);
                    }
                } else if (status == "budget_exhausted") {
                    exhausted = true;
                    all_failures_blocked = false;
                } else {
                    all_failures_blocked = all_failures_blocked && blocked(status);
                    if (blocked(status))
                        summary["blocked_provider_attempts"] =
                            json_uint(summary, "blocked_provider_attempts") + 1;
                    health.failed(provider, status, response, deadline, cancel);
                }
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception& error) {
                report["status"] = "unavailable";
                report["error"] = error.what();
                all_failures_blocked = false;
            }
            result["providers"].push_back(std::move(report));
            if (found || transport.byte_budget_exhausted())
                break;
        }
        completed += conclusive ? 1 : 0;
        failed += conclusive ? 0 : 1;
        if (!conclusive)
            summary["failed_query_indexes"].push_back(index);
        with_results += found ? 1 : 0;
    }
    summary["queries_completed"] = completed;
    summary["queries_with_results"] = with_results;
    summary["queries_failed"] = failed;
    summary["queries_unattempted"] = queries.size() - attempted;
    summary["fallback_queries"] = fallback;
    if (fallback)
        summary["provider_use_notice"] = discovery_provider("bing_rss").use_notice;
    if (completed < queries.size()) {
        summary["status"] = completed ? "partial"
                            : exhausted || Clock::now() >= deadline || transport.byte_budget_exhausted()
                                ? "budget_exhausted"
                            : all_failures_blocked ? "blocked"
                                                   : "unavailable";
        summary["guidance"] =
            "Discovery did not complete every query. Supply independently verified source URLs "
            "or retry after the reported cooldown; do not infer that matching products or sources do not "
            "exist.";
    }
    result["summary"] = std::move(summary);
    return result;
}
} // namespace devbox::web
