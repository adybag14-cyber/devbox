#include "devbox/research.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <map>
namespace devbox::web {
std::optional<std::uint64_t> retry_after_millis(std::string_view supplied, std::uint64_t now_ms) {
    const auto value = trim(supplied);
    if (value.empty() || value.size() > 1024)
        return {};
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        std::uint64_t seconds = 0;
        for (const auto c : value) {
            const auto digit = static_cast<std::uint64_t>(c - '0');
            if (seconds > (UINT64_MAX - digit) / 10)
                return UINT64_MAX;
            seconds = seconds * 10 + digit;
        }
        return seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000;
    }
    const auto timestamp = curl_getdate(value.c_str(), nullptr);
    if (timestamp < 0)
        return {};
    const auto seconds = static_cast<std::uint64_t>(timestamp);
    if (seconds <= now_ms / 1000)
        return 0;
    return seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000 - now_ms;
}
Json evidence_quality(const Json& sources, const std::vector<std::string>& primary, const Json& discovery) {
    std::map<std::string, std::size_t> hosts;
    std::map<std::string, std::size_t> matches;
    for (const auto& host : primary)
        matches[host] = 0;
    std::size_t documents = 0, primary_documents = 0, hashed = 0, timestamped = 0;
    for (const auto& source : sources) {
        try {
            const auto host =
                lower(Url::parse(json_string(source, "final_url", json_string(source, "url"))).host);
            if (host.empty())
                continue;
            ++documents;
            ++hosts[host];
            bool declared = false;
            for (auto& [domain, count] : matches)
                if (host == domain || host.ends_with("." + domain)) {
                    ++count;
                    declared = true;
                }
            if (declared)
                ++primary_documents;
            if (!json_string(source, "content_sha256").empty())
                ++hashed;
            if (!json_string(source, "checked_at").empty() || !json_string(source, "validated_at").empty() ||
                !json_string(source, "retrieved_at").empty())
                ++timestamped;
        } catch (...) {
        }
    }
    std::size_t largest = 0;
    Json breakdown = Json::object();
    for (const auto& [host, count] : hosts) {
        breakdown[host] = count;
        largest = std::max(largest, count);
    }
    Json missing = Json::array();
    for (const auto& [domain, count] : matches)
        if (!count)
            missing.push_back(domain);
    return Json{{"retrieved_documents", documents},
                {"distinct_hostnames", hosts.size()},
                {"documents_by_hostname", breakdown},
                {"largest_hostname_share",
                 documents ? static_cast<double>(largest) / static_cast<double>(documents) : 0.0},
                {"source_hashes_present", hashed},
                {"checked_timestamps_present", timestamped},
                {"primary_classification", primary.empty() ? "not_declared" : "caller_declared_domains"},
                {"declared_primary_documents", primary_documents},
                {"missing_declared_primary_domains", missing},
                {"publisher_independence", "not_verified"},
                {"incomplete_queries",
                 json_uint(discovery, "queries_failed") + json_uint(discovery, "queries_unattempted")},
                {"requires_source_review", true},
                {"note", "Document count does not establish independent publishers, primary-source status or "
                         "factual agreement."}};
}
} // namespace devbox::web
