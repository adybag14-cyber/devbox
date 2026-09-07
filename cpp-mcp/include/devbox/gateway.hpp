#pragma once
#include "config.hpp"
#include <map>
#include <set>
namespace devbox {
struct HttpRequest {
    std::string method, target, path, query, peer;
    Json headers = Json::object();
    std::string body;
    bool is_local = false;
    std::optional<Json> oauth;
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
