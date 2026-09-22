#include "devbox/provider.hpp"
#include "devbox/research.hpp"
#include "devbox/resource_budget.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <semaphore>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
namespace devbox {
namespace {
struct HttpExchange {
    const ProviderProfile* profile;
    const ProviderRequest* request;
    Cancel cancel;
    std::unique_ptr<ProviderStream> stream;
    std::string body, content_type, retry_after;
    std::exception_ptr failure;
    std::size_t bytes = 0;
    int status = 0;
    bool streaming = false;
};
std::size_t body_data(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& exchange = *static_cast<HttpExchange*>(opaque);
    try {
        if (size && count > SIZE_MAX / size)
            return 0;
        const auto bytes = size * count;
        if (bytes > exchange.request->budget.max_response_bytes - exchange.bytes)
            throw Error("PROVIDER_RESPONSE_BYTE_BUDGET");
        exchange.bytes += bytes;
        if (exchange.cancel)
            exchange.cancel->check();
        if (exchange.status == 200 && exchange.stream &&
            exchange.content_type.starts_with("text/event-stream")) {
            exchange.streaming = true;
            exchange.stream->feed(std::string_view(data, bytes));
        } else
            exchange.body.append(data, bytes);
        return bytes;
    } catch (...) {
        exchange.failure = std::current_exception();
        return 0;
    }
}
std::size_t header_data(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& exchange = *static_cast<HttpExchange*>(opaque);
    try {
        if (size && count > SIZE_MAX / size)
            return 0;
        const auto bytes = size * count;
        if (bytes > 16384)
            return 0;
        const std::string_view line(data, bytes);
        if (line.starts_with("HTTP/")) {
            const auto space = line.find(' ');
            if (space != line.npos && space + 4 <= line.size()) {
                exchange.status = std::stoi(std::string(line.substr(space + 1, 3)));
                exchange.content_type.clear();
                exchange.retry_after.clear();
            }
        } else if (const auto colon = line.find(':'); colon != line.npos) {
            const auto key = lower(std::string(line.substr(0, colon)));
            if (key == "content-type")
                exchange.content_type = lower(trim(line.substr(colon + 1)));
            if (key == "retry-after")
                exchange.retry_after = trim(line.substr(colon + 1));
        }
        return bytes;
    } catch (...) {
        exchange.failure = std::current_exception();
        return 0;
    }
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto& exchange = *static_cast<HttpExchange*>(opaque);
    return exchange.cancel && exchange.cancel->cancelled() ? 1 : 0;
}
curl_socket_t socket_open(void* opaque, curlsocktype purpose, curl_sockaddr* address) noexcept {
    const auto& exchange = *static_cast<HttpExchange*>(opaque);
    try {
        if (purpose != CURLSOCKTYPE_IPCXN)
            return CURL_SOCKET_BAD;
        char text[INET6_ADDRSTRLEN]{};
        unsigned short port = 0;
        if (address->family == AF_INET) {
            const auto* value = reinterpret_cast<const sockaddr_in*>(&address->addr);
            if (!inet_ntop(AF_INET, &value->sin_addr, text, sizeof(text)))
                return CURL_SOCKET_BAD;
            port = ntohs(value->sin_port);
        } else if (address->family == AF_INET6) {
            const auto* value = reinterpret_cast<const sockaddr_in6*>(&address->addr);
            if (!inet_ntop(AF_INET6, &value->sin6_addr, text, sizeof(text)))
                return CURL_SOCKET_BAD;
            port = ntohs(value->sin6_port);
        } else
            return CURL_SOCKET_BAD;
        if (exchange.profile->local) {
            const auto url = Url::parse(exchange.profile->base_url);
            const auto expected = url.port.empty() ? (url.scheme == "https" ? 443 : 80) : std::stoi(url.port);
            if ((std::string_view(text) != "127.0.0.1" && std::string_view(text) != "::1") ||
                port != expected)
                return CURL_SOCKET_BAD;
        } else if (!web::public_address(text))
            return CURL_SOCKET_BAD;
        return ::socket(address->family, address->socktype, address->protocol);
    } catch (...) {
        return CURL_SOCKET_BAD;
    }
}
ProviderResult exchange(const ProviderProfile& profile, const ProviderRequest& request,
                        std::string_view endpoint, std::string_view body, std::string_view secret,
                        const Cancel& cancel, const std::function<void(std::string_view)>& delta,
                        bool get = false) {
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!initialized)
        throw Error("PROVIDER_TRANSPORT_INITIALIZATION_FAILED");
    auto* curl = curl_easy_init();
    if (!curl)
        throw Error("PROVIDER_TRANSPORT_ALLOCATION_FAILED");
    ScopeExit release([&] { curl_easy_cleanup(curl); });
    HttpExchange state{&profile, &request, cancel, {}, {}, {}, {}, {}, 0, 0, false};
    // A keyed response is not emitted incrementally: buffer and inspect it before any provider
    // content reaches events or model context. Unkeyed local inference can emit live text deltas.
    state.stream =
        std::make_unique<ProviderStream>(profile.protocol, request.budget.max_response_bytes,
                                         secret.empty() ? delta : std::function<void(std::string_view)>{});
    const auto url = profile.base_url + std::string(endpoint);
    curl_slist* headers = nullptr;
    ScopeExit release_headers([&] { curl_slist_free_all(headers); });
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, request.stream ? "Accept: text/event-stream, application/json"
                                                        : "Accept: application/json");
    std::string authorization;
    ScopeExit clear_authorization([&] {
        volatile char* p = authorization.data();
        for (std::size_t i = 0; i < authorization.size(); ++i)
            p[i] = 0;
    });
    if (!secret.empty()) {
        if (!std::all_of(secret.begin(), secret.end(), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '~' || c == '+' || c == '/' || c == '=';
            }))
            throw Error("PROVIDER_INVALID_AUTH_MATERIAL");
        if (body.find(secret) != body.npos)
            throw Error("PROVIDER_SECRET_IN_MODEL_INPUT_DENIED");
        authorization = "Authorization: Bearer " + std::string(secret);
        headers = curl_slist_append(headers, authorization.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, profile.local ? "http,https" : "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    std::optional<std::string> ca;
    if (const auto path = tls_ca_bundle()) {
        ca = path_text(*path);
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca->c_str());
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.budget.deadline.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(std::min<Millis::rep>(5000, request.budget.deadline.count())));
    if (!get) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_data);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, socket_open);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    const auto code = curl_easy_perform(curl);
    ProviderResult result;
    result.http_status = state.status;
    result.response_id = state.stream->response_id();
    if (!secret.empty() && result.response_id.find(secret) != std::string::npos)
        result.response_id.clear();
    curl_off_t uploaded = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    result.billing_unknown = uploaded > 0;
    if (cancel && cancel->cancelled()) {
        result.status = "cancel_requested";
        return result;
    }
    if (state.failure) {
        result.status = "protocol_error";
        result.error_code = "PROVIDER_STREAM_OR_BYTE_BUDGET";
        return result;
    }
    if (code != CURLE_OK) {
        result.status = "transport_uncertain";
        result.error_code = "CURL_" + std::to_string(code);
        return result;
    }
    if (state.status == 429 || state.status == 503) {
        result.status = "rate_limited";
        result.retry_after_ms = web::retry_after_millis(state.retry_after).value_or(60000);
        return result;
    }
    if (state.status != 200) {
        result.status = "rejected";
        result.error_code = "HTTP_" + std::to_string(state.status);
        return result;
    }
    try {
        if (state.streaming)
            result = state.stream->finish();
        else {
            std::size_t nodes = 0;
            const auto value = Json::parse(state.body, [&](int depth, Json::parse_event_t, Json&) {
                if (depth > 64 || ++nodes > 131072)
                    throw Error("PROVIDER_JSON_SHAPE_BUDGET");
                return true;
            });
            result = parse_provider_response(profile.protocol, value);
        }
        result.http_status = state.status;
        if (!secret.empty() && result.json().dump().find(secret) != std::string::npos) {
            result = {};
            result.status = "protocol_error";
            result.error_code = "PROVIDER_SECRET_ECHO_DENIED";
        } else if (!secret.empty() && delta && !result.text.empty())
            delta(result.text);
    } catch (...) {
        result = {};
        result.status = "protocol_error";
        result.error_code = "PROVIDER_INVALID_RESPONSE";
    }
    return result;
}
} // namespace
ModelProvider::ModelProvider(ProviderProfile profile, SecretBroker* secrets)
    : profile_(std::move(profile)), secrets_(secrets) {
    while (profile_.base_url.ends_with('/'))
        profile_.base_url.pop_back();
    const auto url = Url::parse(profile_.base_url);
    if (profile_.base_url.size() > 2048 || !url.has_authority || !url.userinfo.empty() || url.has_query ||
        url.has_fragment)
        throw Error("PROVIDER_ENDPOINT_INVALID");
    if (profile_.local) {
        if ((url.scheme != "http" && url.scheme != "https") ||
            (url.host != "127.0.0.1" && url.host != "::1" && url.host != "[::1]"))
            throw Error("PROVIDER_LOCAL_ENDPOINT_MUST_BE_LITERAL_LOOPBACK");
    } else if (url.scheme != "https")
        throw Error("PROVIDER_REMOTE_HTTPS_REQUIRED");
    if (!profile_.local && !profile_.secret_reference)
        throw Error("PROVIDER_SCOPED_CREDENTIAL_REQUIRED");
    if (profile_.secret_reference && !secrets_)
        throw Error("PROVIDER_SECRET_BROKER_REQUIRED");
}
Json ModelProvider::capabilities() const {
    return Json{
        {"id", profile_.id},
        {"model", profile_.model},
        {"protocol", profile_.protocol == ProviderProtocol::Responses ? "responses" : "chat_completions"},
        {"tools", profile_.tools},
        {"vision", profile_.vision},
        {"streaming", profile_.streaming},
        {"background_cancel", profile_.background_cancel},
        {"context_tokens", profile_.context_tokens},
        {"max_output_tokens", profile_.output_tokens},
        {"capability_source", "operator_configured"},
        {"credentials_exposed_to_model", false},
        {"automatic_request_retries", false},
        {"keyed_text_deltas", "buffered_until_secret_echo_check"}};
}
ProviderResult ModelProvider::generate(const ProviderRequest& request, const SecretScope& scope,
                                       const Cancel& cancel, std::function<void(std::string_view)> delta) {
    static std::counting_semaphore<2> capacity(2);
    if (!capacity.try_acquire())
        throw Error("PROVIDER_CAPACITY");
    ScopeExit release([&] { capacity.release(); });
    if (scope.principal.empty() || scope.run.empty() || scope.destination != profile_.base_url)
        throw Error("PROVIDER_SCOPE_MISMATCH");
    if (cancel)
        cancel->check();
    const auto payload =
        bounded_json_dump(provider_payload(profile_, request), request.budget.max_request_bytes);
    ProviderResult result;
    const auto invoke = [&](std::string_view key) {
        result =
            exchange(profile_, request,
                     profile_.protocol == ProviderProtocol::Responses ? "/responses" : "/chat/completions",
                     payload, trim(key), cancel, delta);
    };
    if (profile_.secret_reference)
        secrets_->use(*profile_.secret_reference, scope, invoke);
    else
        invoke({});
    if (!profile_.tools && !result.calls.empty()) {
        result.calls.clear();
        result.status = "protocol_error";
        result.error_code = "PROVIDER_UNNEGOTIATED_TOOLS";
    }
    if (std::any_of(result.calls.begin(), result.calls.end(), [&](const auto& call) {
            return std::none_of(request.tools.begin(), request.tools.end(), [&](const auto& tool) {
                return tool.contains("function") && json_string(tool["function"], "name") == call.name;
            });
        })) {
        result.calls.clear();
        result.status = "protocol_error";
        result.error_code = "PROVIDER_UNDECLARED_TOOL";
    }
    if (result.usage.input_tokens && result.usage.output_tokens) {
        const auto input = *result.usage.input_tokens, output = *result.usage.output_tokens;
        if (input > request.budget.max_total_tokens || output > request.budget.max_total_tokens - input ||
            output > request.budget.max_output_tokens) {
            result.status = "budget_exceeded";
            result.calls.clear();
            result.error_code = "PROVIDER_REPORTED_TOKEN_BUDGET_EXCEEDED";
        }
        result.usage.cost_ceiling_micro_usd = provider_cost_ceiling(profile_, input, output);
        if (*result.usage.cost_ceiling_micro_usd > request.budget.max_cost_micro_usd) {
            result.status = "budget_exceeded";
            result.calls.clear();
            result.error_code = "PROVIDER_REPORTED_COST_BUDGET_EXCEEDED";
        }
    }
    return result;
}
ProviderResult ModelProvider::cancel_background(std::string_view response_id, const SecretScope& scope,
                                                const Cancel& cancel) {
    if (!profile_.background_cancel || profile_.protocol != ProviderProtocol::Responses)
        throw Error("PROVIDER_REMOTE_CANCEL_UNSUPPORTED: cancelling a foreground transport does not confirm "
                    "remote termination or billing");
    if (scope.destination != profile_.base_url || scope.principal.empty() || scope.run.empty() ||
        response_id.empty() || response_id.size() > 128 ||
        !std::all_of(response_id.begin(), response_id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                   c == '-';
        }))
        throw Error("PROVIDER_CANCEL_SCOPE_OR_ID_INVALID");
    ProviderRequest request;
    request.stream = false;
    request.budget.deadline = Millis(10000);
    ProviderResult result;
    const auto invoke = [&](std::string_view key) {
        result = exchange(profile_, request, "/responses/" + std::string(response_id) + "/cancel", "{}",
                          trim(key), cancel, {});
    };
    if (profile_.secret_reference)
        secrets_->use(*profile_.secret_reference, scope, invoke);
    else
        invoke({});
    result.remote_cancel_confirmed = result.status == "cancelled";
    return result;
}
ProviderResult ModelProvider::retrieve(std::string_view id, const SecretScope& scope, const Cancel& cancel) {
    if (profile_.protocol != ProviderProtocol::Responses || scope.destination != profile_.base_url ||
        scope.principal.empty() || scope.run.empty() || id.empty() || id.size() > 128 ||
        !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                   c == '-';
        }))
        throw Error("PROVIDER_RETRIEVE_SCOPE_OR_ID_INVALID");
    ProviderRequest request;
    request.stream = false;
    request.budget.deadline = Millis(10000);
    ProviderResult result;
    const auto invoke = [&](std::string_view key) {
        result =
            exchange(profile_, request, "/responses/" + std::string(id), {}, trim(key), cancel, {}, true);
    };
    if (profile_.secret_reference)
        secrets_->use(*profile_.secret_reference, scope, invoke);
    else
        invoke({});
    return result;
}
} // namespace devbox
