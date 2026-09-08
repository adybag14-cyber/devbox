#include "devbox/oauth.hpp"
#include <algorithm>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <set>

namespace devbox {
namespace {
constexpr std::uint64_t access_ttl = 3600000, refresh_ttl = 7 * 24 * access_ttl, code_ttl = 600000,
                        secret_ttl = 30 * 24 * 3600;
Json optional_value(const Json& value, std::string_view key) {
    auto it = value.find(std::string(key));
    return it == value.end() ? Json(nullptr) : *it;
}
bool string_array(const Json& value) {
    return value.is_array() &&
           std::all_of(value.begin(), value.end(), [](const auto& v) { return v.is_string(); });
}
Json scopes_from(const Json& request) {
    return split(json_string(request, "scope"), ' ', false);
}
bool safe_url(std::string_view value) {
    try {
        const auto scheme = Url::parse(value).scheme;
        return scheme != "javascript" && scheme != "data" && scheme != "vbscript";
    } catch (...) {
        return false;
    }
}
void validate_metadata(Json& metadata) {
    if (!metadata.is_object())
        throw OAuthFailure("invalid_client_metadata", "Client metadata must be a JSON object");
    const std::set<std::string> allowed = {"redirect_uris",
                                           "token_endpoint_auth_method",
                                           "grant_types",
                                           "response_types",
                                           "client_name",
                                           "client_uri",
                                           "logo_uri",
                                           "scope",
                                           "contacts",
                                           "tos_uri",
                                           "policy_uri",
                                           "jwks_uri",
                                           "jwks",
                                           "software_id",
                                           "software_version",
                                           "software_statement"};
    for (auto it = metadata.begin(); it != metadata.end();) {
        if (!allowed.contains(it.key()))
            it = metadata.erase(it);
        else
            ++it;
    }
    const auto redirects = optional_value(metadata, "redirect_uris");
    if (!string_array(redirects))
        throw OAuthFailure("invalid_client_metadata", "redirect_uris must be an array of URLs");
    if (redirects.empty() || std::any_of(redirects.begin(), redirects.end(), [](const auto& uri) {
            return !safe_url(uri.template get<std::string>());
        }))
        throw OAuthFailure("invalid_client_metadata", "redirect_uris must contain safe, valid URLs");
    for (const auto field : {"token_endpoint_auth_method", "client_name", "scope", "policy_uri",
                             "software_id", "software_version", "software_statement"}) {
        if (metadata.contains(field) && !metadata[field].is_string())
            throw OAuthFailure("invalid_client_metadata", std::string(field) + " must be a string");
    }
    for (const auto field : {"grant_types", "response_types", "contacts"}) {
        if (metadata.contains(field) && !string_array(metadata[field]))
            throw OAuthFailure("invalid_client_metadata",
                               std::string(field) + " must be an array of strings");
    }
    for (const auto field : {"client_uri", "logo_uri", "tos_uri", "jwks_uri"}) {
        if (!metadata.contains(field))
            continue;
        if (!metadata[field].is_string() ||
            (!metadata[field].template get_ref<const std::string&>().empty() &&
             !safe_url(metadata[field].template get_ref<const std::string&>())))
            throw OAuthFailure("invalid_client_metadata", std::string(field) + " must be a safe, valid URL");
        if (metadata[field] == "")
            metadata.erase(field);
    }
}
std::string resolve_redirect(const Json& request, const Json& client) {
    const auto registered = optional_value(client, "redirect_uris");
    if (!string_array(registered))
        throw OAuthFailure("invalid_request", "Registered client has no redirect_uris");
    if (request.contains("redirect_uri")) {
        const auto requested = json_string(request, "redirect_uri");
        for (const auto& item : registered)
            if (redirect_uri_matches(requested, item.get<std::string>()))
                return requested;
        throw OAuthFailure("invalid_request", "Unregistered redirect_uri");
    }
    if (registered.size() == 1)
        return registered[0].get<std::string>();
    throw OAuthFailure("invalid_request",
                       "redirect_uri must be specified when client has multiple registered URIs");
}
Json parse_jwt_part(std::string_view part) {
    const auto bytes = base64_decode(part, true);
    return Json::parse(bytes.begin(), bytes.end());
}
void verify_rsa_signature(const Json& jwk, std::string_view message, std::string_view signature) {
    if (json_string(jwk, "kty") != "RSA")
        throw Error("JWK is not an RSA key");
    const auto n_bytes = base64_decode(json_string(jwk, "n"), true);
    const auto e_bytes = base64_decode(json_string(jwk, "e"), true);
    if (n_bytes.empty() || e_bytes.empty() || n_bytes.size() > 16384 || e_bytes.size() > 16)
        throw Error("Invalid RSA key parameters");
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> n(
        BN_bin2bn(n_bytes.data(), static_cast<int>(n_bytes.size()), nullptr), BN_free),
        e(BN_bin2bn(e_bytes.data(), static_cast<int>(e_bytes.size()), nullptr), BN_free);
    const std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(),
                                                                                  OSSL_PARAM_BLD_free);
    if (!n || !e || !builder || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, n.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, e.get()) != 1)
        throw Error("Unable to create RSA parameters");
    const std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(
        OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* raw_key = nullptr;
    if (!params || !key_context || EVP_PKEY_fromdata_init(key_context.get()) != 1 ||
        EVP_PKEY_fromdata(key_context.get(), &raw_key, EVP_PKEY_PUBLIC_KEY, params.get()) != 1)
        throw Error("Invalid RSA public key");
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    const auto signature_bytes = base64_decode(signature, true);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestVerify(context.get(), signature_bytes.data(), signature_bytes.size(),
                         reinterpret_cast<const unsigned char*>(message.data()), message.size()) != 1)
        throw Error("Invalid signature");
}
} // namespace

OAuthFailure::OAuthFailure(std::string c, std::string message, int s)
    : Error(std::move(message)), code(std::move(c)), status(s) {}
OAuthFailure OAuthFailure::redirected(std::string uri) const {
    auto result = *this;
    result.safe_redirect_uri = std::move(uri);
    return result;
}
Json OAuthFailure::response() const {
    return Json{{"error", code}, {"error_description", what()}};
}
std::string pkce_challenge(std::string_view verifier) {
    const auto digest = sha256(verifier);
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < digest.size(); i += 2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(digest.substr(i, 2), nullptr, 16)));
    return base64_encode(bytes, true);
}
bool redirect_uri_matches(std::string_view requested, std::string_view registered) {
    if (requested == registered)
        return true;
    try {
        const auto a = Url::parse(requested), b = Url::parse(registered);
        const auto loopback = [](const auto& h) {
            return h == "localhost" || h == "127.0.0.1" || h == "::1";
        };
        return loopback(a.host) && loopback(b.host) && a.scheme == b.scheme && a.host == b.host &&
               a.path == b.path && a.query == b.query && a.has_query == b.has_query;
    } catch (...) {
        return false;
    }
}
std::string append_query(std::string_view uri, const Json& pairs) {
    auto parsed = Url::parse(uri);
    for (auto it = pairs.begin(); it != pairs.end(); ++it) {
        if (!parsed.query.empty())
            parsed.query += '&';
        parsed.query += url_encode(it.key()) + "=" + url_encode(it.value().get<std::string>());
    }
    parsed.has_query = true;
    return parsed.str();
}
OAuthService::OAuthService(const Config& config) : config_(config) {
    if (!enabled(config))
        throw Error("OAuth requires a configured public base URL and auth mode");
}
bool OAuthService::enabled(const Config& config) {
    return config.auth_mode != AuthMode::none && config.public_base_url.has_value();
}
std::string OAuthService::endpoint(std::string_view suffix) const {
    auto url = Url::parse(*config_.public_base_url);
    url.path = "/" + std::string(suffix.starts_with('/') ? suffix.substr(1) : suffix);
    url.query.clear();
    url.fragment.clear();
    url.has_query = false;
    url.has_fragment = false;
    return url.str();
}
Json OAuthService::authorization_server_metadata() const {
    return Json{
        {"issuer", Url::parse(*config_.public_base_url).str()},
        {"authorization_endpoint", endpoint("authorize")},
        {"response_types_supported", {"code"}},
        {"code_challenge_methods_supported", {"S256"}},
        {"token_endpoint", endpoint("token")},
        {"token_endpoint_auth_methods_supported", {"client_secret_post", "none"}},
        {"grant_types_supported", {"authorization_code", "refresh_token"}},
        {"scopes_supported",
         {"mcp:tools", "mcp:devbox:read", "mcp:devbox:exec", "mcp:host:read", "mcp:host:exec", "mcp:admin"}},
        {"revocation_endpoint", endpoint("revoke")},
        {"revocation_endpoint_auth_methods_supported", {"client_secret_post"}},
        {"registration_endpoint", endpoint("register")}};
}
Json OAuthService::protected_resource_metadata(bool legacy) const {
    return Json{
        {"resource", endpoint(legacy ? "mcp" : "")},
        {"authorization_servers", {Url::parse(*config_.public_base_url).str()}},
        {"scopes_supported",
         {"mcp:tools", "mcp:devbox:read", "mcp:devbox:exec", "mcp:host:read", "mcp:host:exec", "mcp:admin"}},
        {"resource_name", legacy ? "Docker ChatGPT Devbox MCP" : config_.server_name()}};
}
std::string OAuthService::resource_metadata_url(bool legacy) const {
    return endpoint(legacy ? ".well-known/oauth-protected-resource/mcp"
                           : ".well-known/oauth-protected-resource");
}
Json OAuthService::oauth_info() const {
    return Json{{"issuer", Url::parse(*config_.public_base_url).str()},
                {"resourceMetadataUrl", resource_metadata_url()},
                {"legacyResourceMetadataUrl", resource_metadata_url(true)}};
}
void OAuthService::load_locked() {
    if (loaded_)
        return;
    try {
        if (auto disk = read_json_optional(config_.oauth_state_file_path, 64 * 1024 * 1024)) {
            if (!disk->is_object())
                throw Error("OAuth state must be an object");
            const auto load = [&](std::string_view name, Records& records) {
                const auto entries = disk->value(std::string(name), Json::array());
                if (!entries.is_array())
                    throw Error("OAuth state records must be arrays");
                Records incoming;
                for (const auto& pair : entries) {
                    if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_object())
                        throw Error("Malformed OAuth state record");
                    if (name != "clients" &&
                        (!pair[1].contains("expiresAt") || !pair[1]["expiresAt"].is_number_unsigned() ||
                         !pair[1].contains("clientId") || !pair[1]["clientId"].is_string()))
                        throw Error("Malformed OAuth token record");
                    incoming[pair[0].get<std::string>()] = pair[1];
                }
                records = std::move(incoming);
            };
            load("clients", clients_);
            load("authorizationCodes", codes_);
            load("accessTokens", access_);
            load("refreshTokens", refresh_);
        }
        loaded_ = true;
        if (prune_locked())
            persist_locked();
    } catch (const OAuthFailure&) {
        throw;
    } catch (const std::exception& e) {
        loaded_ = false;
        throw OAuthFailure("server_error", e.what(), 500);
    }
}
void OAuthService::persist_locked() {
    Json disk = Json::object();
    const auto save = [&](const char* key, const Records& records) {
        auto& items = disk[key] = Json::array();
        for (const auto& [name, record] : records)
            items.push_back(Json::array({name, record}));
    };
    save("clients", clients_);
    save("authorizationCodes", codes_);
    save("accessTokens", access_);
    save("refreshTokens", refresh_);
    try {
        write_json_atomic(config_.oauth_state_file_path, disk);
    } catch (const std::exception& e) {
        throw OAuthFailure("server_error", e.what(), 500);
    }
}
bool OAuthService::prune_locked() {
    const auto now = unix_millis();
    auto count = std::erase_if(
        codes_, [now](const auto& entry) { return json_uint(entry.second, "expiresAt") <= now; });
    count += std::erase_if(access_,
                           [now](const auto& entry) { return json_uint(entry.second, "expiresAt") <= now; });
    count += std::erase_if(refresh_,
                           [now](const auto& entry) { return json_uint(entry.second, "expiresAt") <= now; });
    return count > 0;
}
void OAuthService::prune_clients_locked() {
    const auto limit = std::max<std::size_t>(1, config_.oauth_max_clients);
    if (clients_.size() < limit)
        return;
    std::set<std::string> referenced;
    for (const auto* records : {&codes_, &access_, &refresh_})
        for (const auto& [key, item] : *records) {
            (void)key;
            referenced.insert(json_string(item, "clientId"));
        }
    std::vector<std::pair<std::uint64_t, std::string>> candidates;
    for (const auto& [id, record] : clients_)
        if (!referenced.contains(id))
            candidates.emplace_back(json_uint(record, "client_id_issued_at"), id);
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [issued, id] : candidates) {
        (void)issued;
        if (clients_.size() < limit)
            break;
        clients_.erase(id);
    }
}
Json OAuthService::register_client(Json metadata) {
    validate_metadata(metadata);
    const auto id = uuid();
    const auto issued = unix_millis() / 1000;
    metadata["client_id"] = id;
    metadata["client_id_issued_at"] = issued;
    if (json_string(metadata, "token_endpoint_auth_method") != "none") {
        metadata["client_secret"] = base64_encode(random_bytes(32), true);
        metadata["client_secret_expires_at"] = issued + secret_ttl;
    }
    std::lock_guard lock(state_mutex_);
    load_locked();
    prune_clients_locked();
    if (clients_.size() >= std::max<std::size_t>(1, config_.oauth_max_clients))
        throw OAuthFailure("invalid_client_metadata",
                           "OAuth client registry capacity is occupied by active clients; retry after older "
                           "sessions expire or are revoked.");
    clients_[id] = metadata;
    persist_locked();
    return metadata;
}
std::string OAuthService::authorize(const Json& request, std::optional<std::string> assertion,
                                    std::optional<std::string> email, const Cancel& cancel) {
    const auto id = json_string(request, "client_id");
    Json client;
    {
        std::lock_guard lock(state_mutex_);
        load_locked();
        const auto found = clients_.find(id);
        if (found == clients_.end())
            throw OAuthFailure("invalid_client", "Invalid client_id");
        client = found->second;
    }
    const auto redirect = resolve_redirect(request, client);
    if (json_string(request, "response_type") != "code")
        throw OAuthFailure("invalid_request", "response_type must be code").redirected(redirect);
    if (json_string(request, "code_challenge").empty() ||
        json_string(request, "code_challenge_method") != "S256")
        throw OAuthFailure("invalid_request", "code_challenge and S256 are required").redirected(redirect);
    if (request.contains("resource")) {
        try {
            (void)Url::parse(json_string(request, "resource"));
        } catch (...) {
            throw OAuthFailure("invalid_request", "resource must be a valid URL").redirected(redirect);
        }
    }
    Json identity = nullptr;
    if (config_.auth_mode == AuthMode::cloudflare_access) {
        try {
            identity = verify_identity(assertion, email, cancel);
        } catch (const OAuthFailure& e) {
            throw e.redirected(redirect);
        }
    }
    if (cancel)
        cancel->check();
    std::lock_guard lock(state_mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end() || found->second != client)
        throw OAuthFailure("invalid_client", "Client changed during authorization");
    const auto code = uuid();
    Json params{{"scopes", scopes_from(request)},
                {"redirectUri", redirect},
                {"codeChallenge", json_string(request, "code_challenge")}};
    for (const auto key : {"state", "resource"})
        if (request.contains(key))
            params[key] = request[key];
    Json record{{"clientId", id}, {"expiresAt", unix_millis() + code_ttl}, {"params", params}};
    if (!identity.is_null())
        record["identity"] = identity;
    codes_[code] = record;
    try {
        persist_locked();
    } catch (const OAuthFailure& e) {
        throw e.redirected(redirect);
    }
    Json pairs{{"code", code}};
    if (request.contains("state"))
        pairs["state"] = request["state"];
    return append_query(redirect, pairs);
}
const Json& OAuthService::authenticate_locked(const Json& request) {
    const auto found = clients_.find(json_string(request, "client_id"));
    if (found == clients_.end())
        throw OAuthFailure("invalid_client", "Invalid client_id");
    const auto& client = found->second;
    if (client.contains("client_secret") && client["client_secret"].is_string()) {
        if (!request.contains("client_secret"))
            throw OAuthFailure("invalid_client", "Client secret is required");
        if (!constant_time_equal(json_string(request, "client_secret"), json_string(client, "client_secret")))
            throw OAuthFailure("invalid_client", "Invalid client_secret");
        if (client.contains("client_secret_expires_at") &&
            json_uint(client, "client_secret_expires_at") < unix_millis() / 1000)
            throw OAuthFailure("invalid_client", "Client secret has expired");
    }
    return client;
}
Json OAuthService::issue_locked(const std::string& client, const Json& scopes, const Json& resource,
                                const Json& identity) {
    const auto access = uuid(), refresh = uuid();
    Json record{{"clientId", client}, {"scopes", scopes}, {"expiresAt", unix_millis() + access_ttl}};
    if (!resource.is_null())
        record["resource"] = resource;
    if (!identity.is_null())
        record["identity"] = identity;
    access_[access] = record;
    record["expiresAt"] = unix_millis() + refresh_ttl;
    refresh_[refresh] = record;
    return Json{{"access_token", access},
                {"token_type", "bearer"},
                {"expires_in", 3600},
                {"refresh_token", refresh},
                {"scope", join(scopes.get<std::vector<std::string>>(), " ")}};
}
Json OAuthService::exchange_token(const Json& request) {
    std::lock_guard lock(state_mutex_);
    load_locked();
    (void)authenticate_locked(request);
    const auto client = json_string(request, "client_id"), grant = json_string(request, "grant_type");
    Json scopes, resource, identity;
    if (grant == "authorization_code") {
        if (!request.contains("code"))
            throw OAuthFailure("invalid_request", "code is required");
        if (!request.contains("code_verifier"))
            throw OAuthFailure("invalid_request", "code_verifier is required");
        const auto code = json_string(request, "code");
        const auto found = codes_.find(code);
        if (found == codes_.end())
            throw OAuthFailure("invalid_grant", "Invalid authorization code");
        const auto record = found->second;
        if (json_uint(record, "expiresAt") <= unix_millis()) {
            codes_.erase(found);
            persist_locked();
            throw OAuthFailure("invalid_grant", "Authorization code expired");
        }
        if (json_string(record, "clientId") != client)
            throw OAuthFailure("invalid_grant", "Authorization code was not issued to this client");
        const auto& params = record.at("params");
        if (!constant_time_equal(pkce_challenge(json_string(request, "code_verifier")),
                                 json_string(params, "codeChallenge")))
            throw OAuthFailure("invalid_grant", "code_verifier does not match the challenge");
        if (request.contains("redirect_uri") &&
            json_string(request, "redirect_uri") != json_string(params, "redirectUri"))
            throw OAuthFailure("invalid_grant", "redirect_uri does not match the authorization request");
        scopes = params.value("scopes", Json::array());
        resource = request.contains("resource") ? request["resource"] : optional_value(params, "resource");
        identity = optional_value(record, "identity");
        codes_.erase(found);
    } else if (grant == "refresh_token") {
        if (!request.contains("refresh_token"))
            throw OAuthFailure("invalid_request", "refresh_token is required");
        const auto token = json_string(request, "refresh_token");
        const auto found = refresh_.find(token);
        if (found == refresh_.end())
            throw OAuthFailure("invalid_grant", "Invalid refresh token");
        const auto record = found->second;
        if (json_uint(record, "expiresAt") <= unix_millis()) {
            refresh_.erase(found);
            persist_locked();
            throw OAuthFailure("invalid_grant", "Refresh token expired");
        }
        if (json_string(record, "clientId") != client)
            throw OAuthFailure("invalid_grant", "Refresh token was not issued to this client");
        scopes = scopes_from(request);
        if (scopes.empty())
            scopes = record.value("scopes", Json::array());
        resource = request.contains("resource") ? request["resource"] : optional_value(record, "resource");
        identity = optional_value(record, "identity");
        refresh_.erase(found);
    } else
        throw OAuthFailure("unsupported_grant_type",
                           "The grant type is not supported by this authorization server.");
    auto result = issue_locked(client, scopes, resource, identity);
    persist_locked();
    return result;
}
void OAuthService::revoke(const Json& request) {
    if (!request.contains("token"))
        throw OAuthFailure("invalid_request", "token is required");
    std::lock_guard lock(state_mutex_);
    load_locked();
    (void)authenticate_locked(request);
    const auto token = json_string(request, "token");
    access_.erase(token);
    refresh_.erase(token);
    persist_locked();
}
Json OAuthService::verify_access_token(std::string_view token) {
    std::lock_guard lock(state_mutex_);
    load_locked();
    const auto found = access_.find(std::string(token));
    if (found == access_.end())
        throw OAuthFailure("invalid_token", "Unknown access token", 401);
    const auto record = found->second;
    if (json_uint(record, "expiresAt") <= unix_millis()) {
        access_.erase(found);
        persist_locked();
        throw OAuthFailure("invalid_token", "Access token expired", 401);
    }
    Json result = record;
    result["expiresAt"] = json_uint(record, "expiresAt") / 1000;
    return result;
}
std::optional<Json> OAuthService::cached_key(std::string_view kid) {
    std::lock_guard lock(cache_mutex_);
    if (!jwks_fetched_at_ || unix_millis() - std::min(unix_millis(), jwks_fetched_at_) >= 300000)
        return {};
    for (const auto& key : jwks_.value("keys", Json::array()))
        if (json_string(key, "kid") == kid)
            return std::optional<Json>{std::in_place, key};
    return {};
}
Json OAuthService::decoding_key(std::string_view kid, const Cancel& cancel) {
    if (auto key = cached_key(kid))
        return *key;
    std::unique_lock lock(refresh_mutex_, std::defer_lock);
    const auto deadline = Clock::now() + Millis(10000);
    while (!lock.try_lock_for(Millis(25))) {
        if (cancel)
            cancel->check();
        if (Clock::now() >= deadline)
            throw OAuthFailure("server_error", "Cloudflare key refresh wait timed out", 500);
    }
    if (auto key = cached_key(kid))
        return *key;
    if (last_refresh_ && Clock::now() - *last_refresh_ < Millis(5000))
        throw OAuthFailure("server_error", "Cloudflare key refresh is cooling down; retry shortly", 500);
    last_refresh_ = Clock::now();
    const auto url = config_.cloudflare_access_jwks_url.value_or(
        config_.cloudflare_access_team_domain.value_or("") + "/cdn-cgi/access/certs");
    try {
        const auto result = http_request("GET", url, {}, Json::object(), Millis(10000), 256 * 1024, cancel);
        if (result.status < 200 || result.status >= 300)
            throw Error("JWKS returned HTTP " + std::to_string(result.status));
        const auto set = Json::parse(result.body);
        if (!set.is_object() || !set.contains("keys") || !set["keys"].is_array())
            throw Error("Invalid JWKS document");
        {
            std::lock_guard cache_lock(cache_mutex_);
            jwks_ = set;
            jwks_fetched_at_ = unix_millis();
        }
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        throw OAuthFailure("server_error",
                           std::string("Cloudflare Access JWT verification failed: ") + e.what(), 500);
    }
    if (auto key = cached_key(kid))
        return *key;
    throw OAuthFailure("server_error", "Cloudflare Access JWT verification failed: unknown kid", 500);
}
Json OAuthService::verify_identity(const std::optional<std::string>& assertion,
                                   const std::optional<std::string>& fallback_email, const Cancel& cancel) {
    if (!assertion)
        throw OAuthFailure("invalid_request", "Cloudflare Access authentication is required on /authorize. "
                                              "Protect that path with a Cloudflare Access application.");
    if (!config_.cloudflare_access_team_domain)
        throw OAuthFailure("server_error", "Cloudflare Access team domain is not configured", 500);
    try {
        if (assertion->size() > 256 * 1024)
            throw Error("JWT exceeds the byte limit");
        const auto pieces = split(*assertion, '.');
        if (pieces.size() != 3)
            throw Error("Invalid JWT structure");
        const auto header = parse_jwt_part(pieces[0]);
        if (json_string(header, "alg") != "RS256")
            throw Error("unsupported algorithm " + json_string(header, "alg"));
        const auto kid = json_string(header, "kid");
        if (kid.empty())
            throw Error("missing kid");
        const auto key = decoding_key(kid, cancel);
        verify_rsa_signature(key, pieces[0] + "." + pieces[1], pieces[2]);
        const auto payload = parse_jwt_part(pieces[1]);
        if (!payload.is_object() || !payload.contains("exp") || !payload["exp"].is_number_unsigned())
            throw Error("Missing required claim: exp");
        if (json_uint(payload, "exp") < unix_millis() / 1000 - 60)
            throw Error("ExpiredSignature");
        if (json_string(payload, "iss") != *config_.cloudflare_access_team_domain)
            throw Error("InvalidIssuer");
        const auto aud = optional_value(payload, "aud");
        const auto expected = config_.cloudflare_access_aud;
        if (!(aud.is_string() && aud == expected) &&
            !(string_array(aud) &&
              std::any_of(aud.begin(), aud.end(), [&](const auto& v) { return v == expected; })))
            throw Error("InvalidAudience");
        const auto email = payload.contains("email") && payload["email"].is_string() ? payload["email"]
                           : fallback_email                                          ? Json(*fallback_email)
                                                                                     : Json(nullptr);
        return Json{{"sub", json_string(payload, "sub")},
                    {"email", email},
                    {"name", payload.contains("name") && payload["name"].is_string() ? payload["name"]
                                                                                     : Json(nullptr)},
                    {"aud", aud},
                    {"iss", payload["iss"]}};
    } catch (const OAuthFailure&) {
        throw;
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        throw OAuthFailure("server_error",
                           std::string("Cloudflare Access JWT verification failed: ") + e.what(), 500);
    }
}
std::optional<std::string> required_tool_scope(std::string_view tool) {
    static const std::map<std::string, std::vector<std::string>> groups = {
        {"mcp:devbox:read",
         {"devbox_capabilities", "devbox_file_state", "devbox_job_list", "devbox_task_get",
          "devbox_task_list", "devbox_status", "devbox_github_auth_status", "devbox_job_logs",
          "devbox_job_status", "devbox_list_files", "devbox_read_file", "devbox_read_large_file",
          "devbox_search_files", "devbox_wait", "devbox_wait_for_file"}},
        {"mcp:devbox:exec",
         {"devbox_write_file_atomic", "devbox_job_submit", "devbox_task_put", "devbox_exec",
          "devbox_exec_readonly", "devbox_exec_start", "devbox_job_cancel", "devbox_run_program",
          "devbox_run_program_start", "devbox_write_file", "devbox_write_large_file"}},
        {"mcp:admin",
         {"devbox_recreate", "devbox_restart", "devbox_start", "devbox_stop",
          "devbox_sync_github_auth_from_host"}},
        {"mcp:host:read",
         {"host_capture_display", "host_capture_program", "host_capture_window", "host_status",
          "windows_host_capture_display", "windows_host_capture_program", "windows_host_inspect_file",
          "windows_host_read_large_file", "windows_host_status"}},
        {"mcp:host:exec",
         {"host_exec", "host_run_program", "windows_host_exec", "windows_host_run_program",
          "windows_host_write_large_file"}}};
    for (const auto& [scope, names] : groups)
        if (std::find(names.begin(), names.end(), tool) != names.end())
            return scope;
    return {};
}
bool oauth_scope_allows(const std::vector<std::string>& scopes, std::string_view required) {
    return std::any_of(scopes.begin(), scopes.end(),
                       [&](const auto& scope) { return scope == "mcp:tools" || scope == required; });
}
} // namespace devbox
