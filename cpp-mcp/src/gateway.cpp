#include "devbox/gateway.hpp"
#include <boost/asio/ip/address.hpp>
#include <utility>
namespace devbox {
namespace {
std::optional<std::string> origin(std::string_view text) {
    if (trim(text).empty())
        return {};
    try {
        return Url::parse(text).origin();
    } catch (...) {
        return {};
    }
}
bool loopback(std::string text, bool forwarded = false) {
    text = trim(text);
    if (text.starts_with('[') && text.ends_with(']'))
        text = text.substr(1, text.size() - 2);
    if (forwarded && lower(text) == "localhost")
        return true;
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(text, ec);
    if (ec)
        return false;
    if (address.is_loopback())
        return true;
    if (forwarded && address.is_v6() && address.to_v6().is_v4_mapped())
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).is_loopback();
    return false;
}
HttpReply host_failure(const std::string& message) {
    return HttpReply::json(
        403, Json{{"jsonrpc", "2.0"}, {"error", {{"code", -32000}, {"message", message}}}, {"id", nullptr}});
}
} // namespace
HttpReply HttpReply::json(int status, const Json& value) {
    return HttpReply{status, value.dump(-1, ' ', false, Json::error_handler_t::replace),
                     Json{{"content-type", "application/json"}}};
}
HttpReply HttpReply::text(int status, std::string value) {
    return HttpReply{status, std::move(value), Json{{"content-type", "text/plain; charset=utf-8"}}};
}
Gateway::Gateway(const Config& config) : config_(config), hosts_{"localhost", "127.0.0.1", "::1"} {
    if (config.public_base_url) {
        try {
            hosts_.insert(Url::parse(*config.public_base_url).host);
        } catch (...) {
        }
    }
    for (const auto& value : config.gateway_bridge_origins)
        if (auto normalized = origin(value))
            origins_.insert(*normalized);
}
Json Gateway::bridge_info(bool is_local) const {
    const auto expose = config_.gateway_bridge_enabled && config_.auth_mode == AuthMode::none && is_local;
    return Json{{"enabled", expose},
                {"origins", expose ? Json(config_.gateway_bridge_origins) : Json::array()},
                {"private_network_access", expose}};
}
GatewayDecision Gateway::inspect(const HttpRequest& request) const {
    GatewayDecision result;
    const auto host = json_string(request.headers, "host");
    if (host.empty()) {
        result.response.emplace(host_failure("Missing Host header"));
        return result;
    }
    try {
        const auto url = Url::parse("http://" + host);
        if (!url.userinfo.empty() || url.path != "/" || url.has_query || url.has_fragment)
            throw Error("Invalid authority");
        if (!hosts_.contains(url.host)) {
            result.response.emplace(host_failure("Invalid Host: " + url.host));
            return result;
        }
    } catch (...) {
        result.response.emplace(host_failure("Invalid Host header: " + host));
        return result;
    }
    result.is_local = loopback(request.peer);
    if (result.is_local && request.headers.contains("x-forwarded-for"))
        result.is_local = loopback(split(json_string(request.headers, "x-forwarded-for"), ',')[0], true);
    const auto requested_origin = origin(json_string(request.headers, "origin"));
    const auto expose =
        config_.gateway_bridge_enabled && config_.auth_mode == AuthMode::none && result.is_local;
    const auto allowed = requested_origin && origins_.contains(*requested_origin);
    if (request.method == "OPTIONS" && requested_origin) {
        if (!expose) {
            result.response.emplace(HttpReply::text(405, ""));
            return result;
        }
        if (!allowed) {
            result.response.emplace(HttpReply::json(
                403, Json{{"error", "Origin is not allowed for the local ChatGPT gateway bridge."}}));
            return result;
        }
        result.response.emplace(HttpReply::text(204, ""));
    }
    if (expose && allowed) {
        auto requested_headers = trim(json_string(request.headers, "access-control-request-headers"));
        if (requested_headers.empty())
            requested_headers =
                "authorization, content-type, last-event-id, mcp-protocol-version, mcp-session-id";
        result.response_headers =
            Json{{"access-control-allow-origin", *requested_origin},
                 {"access-control-allow-methods", "DELETE, GET, HEAD, OPTIONS, POST"},
                 {"access-control-allow-headers", requested_headers},
                 {"access-control-expose-headers", "mcp-session-id"},
                 {"access-control-max-age", "600"},
                 {"vary", "Origin, Access-Control-Request-Method, Access-Control-Request-Headers, "
                          "Access-Control-Request-Private-Network"}};
        if (lower(json_string(request.headers, "access-control-request-private-network")) == "true")
            result.response_headers["access-control-allow-private-network"] = "true";
    }
    return result;
}
std::optional<std::string> RequestRegistry::key(const HttpRequest& request, const Json& id) {
    if (!id.is_string() && !id.is_number_integer())
        return {};
    const auto authorization = json_string(request.headers, "authorization");
    return Json::array({authorization.empty() ? "" : sha256(authorization),
                        json_string(request.headers, "mcp-session-id"), request.peer,
                        json_string(request.headers, "user-agent"), id.is_string() ? "string" : "number", id})
        .dump();
}
RequestRegistry::Guard::Guard(RequestRegistry* registry, std::string key, std::string registration)
    : registry_(registry), key_(std::move(key)), registration_(std::move(registration)) {}
RequestRegistry::Guard::Guard(Guard&& other) noexcept
    : registry_(std::exchange(other.registry_, nullptr)), key_(std::move(other.key_)),
      registration_(std::move(other.registration_)) {}
RequestRegistry::Guard& RequestRegistry::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        reset();
        registry_ = std::exchange(other.registry_, nullptr);
        key_ = std::move(other.key_);
        registration_ = std::move(other.registration_);
    }
    return *this;
}
RequestRegistry::Guard::~Guard() {
    reset();
}
void RequestRegistry::Guard::reset() {
    if (!registry_)
        return;
    std::lock_guard lock(registry_->mutex_);
    auto found = registry_->entries_.find(key_);
    if (found != registry_->entries_.end()) {
        found->second.erase(registration_);
        if (found->second.empty())
            registry_->entries_.erase(found);
    }
    registry_ = nullptr;
}
RequestRegistry::Guard RequestRegistry::register_request(const HttpRequest& request, const Json& id,
                                                         Cancel cancel) {
    const auto request_key = key(request, id);
    if (!request_key)
        return {};
    std::lock_guard lock(mutex_);
    std::size_t active = 0;
    for (const auto& [k, entries] : entries_) {
        (void)k;
        active += entries.size();
    }
    if (active >= 512)
        throw Error("Too many active MCP requests; retry shortly.");
    const auto registration = uuid();
    entries_[*request_key][registration] = std::move(cancel);
    return Guard(this, *request_key, registration);
}
std::size_t RequestRegistry::cancel(const HttpRequest& request, const Json& id) {
    const auto request_key = key(request, id);
    if (!request_key)
        return 0;
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(*request_key);
    if (found == entries_.end())
        return 0;
    std::size_t count = 0;
    for (const auto& [registration, token] : found->second) {
        (void)registration;
        if (!token->cancelled()) {
            token->cancel();
            ++count;
        }
    }
    return count;
}
void RequestRegistry::cancel_all() {
    std::lock_guard lock(mutex_);
    for (const auto& [key, entries] : entries_)
        for (const auto& [registration, cancel] : entries) {
            (void)key;
            (void)registration;
            cancel->cancel();
        }
}
std::size_t RequestRegistry::active_count() {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [key, entries] : entries_) {
        (void)key;
        count += entries.size();
    }
    return count;
}
} // namespace devbox
