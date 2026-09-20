#pragma once
#include "config.hpp"
#include "jobs.hpp"
#include <set>

namespace devbox::web {
struct Transfer {
    std::string url, final_url, body, error, completed_at;
    int status = 0;
    Json headers = Json::object();
    std::size_t bytes = 0;
    std::uint64_t duration_ms = 0;
    std::uint64_t completed_unix_ms = 0;
    std::size_t attempts = 0;
};
struct Request {
    std::string url;
    Json headers = Json::object();
};
struct TransportLimits {
    std::size_t concurrency = 4, per_origin = 1, max_body_bytes = 2 * 1024 * 1024;
    std::size_t max_total_bytes = 64 * 1024 * 1024;
    Millis request_timeout{12000};
    // Native fixture injection only. No CLI, environment variable or MCP field exposes it.
    std::optional<unsigned short> fixture_loopback_port;
};
bool public_address(std::string_view address);
std::string normalize_url(std::string_view value, std::string_view base = {});
class Transport {
    struct State;
    std::unique_ptr<State> state_;

  public:
    explicit Transport(TransportLimits limits = {});
    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    std::vector<Transfer> get(const std::vector<Request>& requests, Clock::time_point deadline,
                              const Cancel& cancel = {});
    std::size_t downloaded_bytes() const;
    bool byte_budget_exhausted() const;
    void reset_byte_budget();
};
Json extract_document(const Transfer& response);
Json extract_offer_records(const Json& schemas, const Json& variants, std::string_view base_url);
Json offer_view(const Json& document, std::size_t offset = 0, std::size_t limit = 20);
bool response_requires_challenge(const Transfer& response);
Json parse_search_response(std::string_view provider, const Transfer& response,
                           std::optional<unsigned short> fixture_loopback_port = {});
Json discover_sources(Transport& transport, const Json& plan, const fs::path& health_root,
                      const TransportLimits& limits, Clock::time_point deadline, const Cancel& cancel);
Json exact_term_matches(const Json& document, const std::vector<std::string>& exact_terms);
std::string evidence_excerpt(std::string_view text, std::string_view query, std::size_t max_chars);
bool robots_allowed(std::string_view robots, std::string_view path,
                    std::string_view agent = "devboxresearch");

class ResearchService {
    struct State;
    std::unique_ptr<State> state_;

  public:
    explicit ResearchService(std::shared_ptr<const Config> config, TransportLimits limits = {});
    ~ResearchService();
    Json fetch(const Json& args, const Cancel& cancel = {});
    Json run(const Json& plan, const fs::path& directory, Millis budget, const Cancel& cancel);
};
Json validate_plan(const Json& args, std::optional<unsigned short> fixture_loopback_port = {});
Json research_evidence(const JobStore& jobs, const Json& args);
Json submit_research(JobManager& jobs, const Json& args);
} // namespace devbox::web
