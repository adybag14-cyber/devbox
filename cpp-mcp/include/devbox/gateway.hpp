#pragma once
#include "config.hpp"
#include <map>
#include <set>
namespace devbox {
struct RequestTiming {
    bool started = false, completed = false;
    Clock::time_point start;
    std::uint64_t start_wall_us = 0, finish_wall_us = 0, duration_us = 0;
    std::uint64_t receive_us = 0, parse_us = 0, prepare_us = 0, write_us = 0;
    std::int64_t wall_steady_delta_us = 0;
    void begin(Clock::time_point steady, std::uint64_t wall) {
        *this = RequestTiming{};
        started = true;
        start = steady;
        start_wall_us = wall;
    }
    void finish(Clock::time_point steady, std::uint64_t wall) {
        if (!started)
            return;
        completed = true;
        finish_wall_us = wall;
        duration_us = static_cast<std::uint64_t>(
            std::max(Micros::zero(), std::chrono::duration_cast<Micros>(steady - start)).count());
        wall_steady_delta_us = static_cast<std::int64_t>(wall) - static_cast<std::int64_t>(start_wall_us) -
                               static_cast<std::int64_t>(duration_us);
    }
};
struct HttpRequest {
    std::string method, target, path, query, peer;
    std::string usage_id = uuid(), started_at = utc_now();
    std::string connection_created_at, receive_started_at, finished_at;
    std::string transport_outcome;
    RequestTiming timing;
    Json headers = Json::object();
    std::string body;
    bool is_local = false;
    std::optional<Json> oauth;
    Cancel cancellation;
};
struct HttpReply {
    int status = 200;
    std::string body;
    Json headers = Json::object();
    static HttpReply json(int status, const Json& value);
    static HttpReply text(int status, std::string value);
};
struct GatewayDecision {
    bool is_local = false;
    Json response_headers = Json::object();
    std::optional<HttpReply> response;
};
class Gateway {
    Config config_;
    std::set<std::string> hosts_, origins_;

  public:
    explicit Gateway(const Config& config);
    GatewayDecision inspect(const HttpRequest& request) const;
    Json bridge_info(bool is_local) const;
};
class RequestRegistry {
    using Registrations = std::map<std::string, Cancel>;
    std::mutex mutex_;
    std::map<std::string, Registrations> entries_;
    static std::optional<std::string> key(const HttpRequest& request, const Json& id);

  public:
    class Guard {
        RequestRegistry* registry_ = nullptr;
        std::string key_, registration_;
        friend class RequestRegistry;
        Guard(RequestRegistry* registry, std::string key, std::string registration);

      public:
        Guard() = default;
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;
        Guard(const Guard&) = delete;
        ~Guard();
        void reset();
    };
    Guard register_request(const HttpRequest& request, const Json& id, Cancel cancel);
    std::size_t cancel(const HttpRequest& request, const Json& id);
    void cancel_all();
    std::size_t active_count();
};
} // namespace devbox
