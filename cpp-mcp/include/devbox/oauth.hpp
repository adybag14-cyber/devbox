#pragma once
#include "config.hpp"
#include <map>

namespace devbox {
struct OAuthFailure : Error {
    std::string code;
    int status;
    std::optional<std::string> safe_redirect_uri;
    OAuthFailure(std::string code, std::string message, int status = 400);
    OAuthFailure redirected(std::string uri) const;
    Json response() const;
};
std::string pkce_challenge(std::string_view verifier);
bool redirect_uri_matches(std::string_view requested, std::string_view registered);
std::string append_query(std::string_view uri, const Json& pairs);
std::optional<std::string> required_tool_scope(std::string_view tool);
bool oauth_scope_allows(const std::vector<std::string>& scopes, std::string_view required);

// Each instance owns one state file. Network verification never holds the state mutex.
class OAuthService {
    using Records = std::map<std::string, Json>;
    Config config_;
    std::mutex state_mutex_, cache_mutex_;
    std::timed_mutex refresh_mutex_;
    bool loaded_ = false;
    Records clients_, codes_, access_, refresh_;
    Json jwks_ = Json::object();
    std::uint64_t jwks_fetched_at_ = 0;
    std::optional<Clock::time_point> last_refresh_;
    void load_locked();
    void persist_locked();
    bool prune_locked();
    void prune_clients_locked();
    const Json& authenticate_locked(const Json& request);
    Json issue_locked(const std::string& client, const Json& scopes, const Json& resource,
                      const Json& identity);
    std::optional<Json> cached_key(std::string_view kid);
    Json decoding_key(std::string_view kid, const Cancel& cancel);
    Json verify_identity(const std::optional<std::string>& assertion,
                         const std::optional<std::string>& fallback_email, const Cancel& cancel);

  public:
    explicit OAuthService(const Config& config);
    static bool enabled(const Config& config);
    std::string endpoint(std::string_view suffix) const;
    Json authorization_server_metadata() const;
    Json protected_resource_metadata(bool legacy = false) const;
    Json oauth_info() const;
    std::string resource_metadata_url(bool legacy = false) const;
    Json register_client(Json metadata);
    std::string authorize(const Json& request, std::optional<std::string> assertion = {},
                          std::optional<std::string> email = {}, const Cancel& cancel = {});
    Json exchange_token(const Json& request);
    void revoke(const Json& request);
    Json verify_access_token(std::string_view token);
};
} // namespace devbox
