#include "devbox/native.hpp"
#include "devbox/research.hpp"
#include <ada.h>
#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <cstring>
#include <curl/curl.h>
#include <map>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace devbox::web {
namespace {
bool v4_public(const std::array<unsigned char, 4>& b) {
    return !(b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 ||
             (b[0] == 100 && b[1] >= 64 && b[1] <= 127) || (b[0] == 169 && b[1] == 254) ||
             (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
             (b[0] == 192 && (b[1] == 168 || (b[1] == 0 && b[2] == 0) || (b[1] == 0 && b[2] == 2))) ||
             (b[0] == 198 && (b[1] == 18 || b[1] == 19 || (b[1] == 51 && b[2] == 100))) ||
             (b[0] == 203 && b[1] == 0 && b[2] == 113));
}
bool redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
std::string header(const Json& headers, std::string_view name) {
    return json_string(headers, name);
}
} // namespace
bool public_address(std::string_view address) {
    boost::system::error_code error;
    const auto ip = boost::asio::ip::make_address(std::string(address), error);
    if (error)
        return false;
    if (ip.is_v4())
        return v4_public(ip.to_v4().to_bytes());
    const auto b = ip.to_v6().to_bytes();
    // Global unicast only; reject mapped, transition, documentation and special-use space.
    if ((b[0] & 0xe0) != 0x20)
        return false;
    if ((b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) ||
        (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] < 0x20) || (b[0] == 0x20 && b[1] == 0x02))
        return false;
    return true;
}
std::string normalize_url(std::string_view value, std::string_view base) {
    if (value.empty() || value.size() > 4096 || value.find_first_of("\r\n\0", 0, 3) != value.npos)
        throw Error("WEB_INVALID_URL: invalid or oversized URL");
    auto parent = ada::parse<ada::url_aggregator>(base);
    auto parsed = ada::parse<ada::url_aggregator>(value, parent ? &*parent : nullptr);
    if (!parsed || (parsed->get_protocol() != "http:" && parsed->get_protocol() != "https:") ||
        !parsed->get_username().empty() || !parsed->get_password().empty())
        throw Error("WEB_INVALID_URL: public HTTP(S) without URL credentials is required");
    auto host = lower(std::string(parsed->get_hostname()));
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    if (host.empty() || host == "localhost" || host.ends_with(".localhost") || host.ends_with(".local") ||
        host.ends_with(".internal") || host.ends_with(".invalid") || host.ends_with(".test"))
        throw Error("WEB_PRIVATE_ADDRESS: local or special host rejected");
    auto literal = host;
    if (literal.starts_with('[') && literal.ends_with(']'))
        literal = literal.substr(1, literal.size() - 2);
    boost::system::error_code error;
    (void)boost::asio::ip::make_address(literal, error);
    if (!error && !public_address(literal))
        throw Error("WEB_PRIVATE_ADDRESS: non-public address rejected");
    if (host != parsed->get_hostname() && !parsed->set_hostname(host))
        throw Error("WEB_INVALID_URL: hostname normalization failed");
    const auto port = std::string(parsed->get_port());
    if (!port.empty() && port != "80" && port != "443")
        throw Error("WEB_INVALID_URL: only standard HTTP(S) ports are supported");
    parsed->set_hash("");
    if (parsed->get_href().size() > 4096)
        throw Error("WEB_INVALID_URL: normalized URL exceeds 4096 bytes");
    return std::string(parsed->get_href());
}

struct Transport::State {
    TransportLimits limits;
    CURLM* multi = nullptr;
    std::size_t total_bytes = 0;
    bool byte_budget_exhausted = false;
    std::map<std::string, Clock::time_point> backoff;
    struct Item {
        State* owner;
        CURL* easy = nullptr;
        curl_slist* request_headers = nullptr;
        Transfer response;
        Json headers;
        std::string current, origin;
        std::size_t index = 0, redirects = 0, retries = 0;
        bool denied_address = false, too_large = false;
        Clock::time_point started;
        Clock::time_point ready_at{};
        ~Item() {
            if (easy)
                curl_easy_cleanup(easy);
            if (request_headers)
                curl_slist_free_all(request_headers);
        }
    };
    explicit State(TransportLimits value) : limits(std::move(value)) {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                throw Error("Could not initialize native web transport");
        });
        multi = curl_multi_init();
        if (!multi)
            throw Error("Could not allocate native web transport");
        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(limits.concurrency));
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(limits.per_origin));
    }
    ~State() {
        curl_multi_cleanup(multi);
    }
    std::string checked_url(std::string_view url, std::string_view base = {}) const {
        if (limits.fixture_loopback_port) {
            const auto prefix = "http://127.0.0.1:" + std::to_string(*limits.fixture_loopback_port);
            auto resolved = url.starts_with('/') ? prefix + std::string(url) : std::string(url);
            if (resolved.starts_with(prefix + "/"))
                return resolved;
        }
        return normalize_url(url, base);
    }
    static curl_socket_t open_socket(void* opaque, curlsocktype, curl_sockaddr* address) noexcept {
        auto& item = *static_cast<Item*>(opaque);
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        const void* bytes = nullptr;
        unsigned short port = 0;
        if (address->family == AF_INET) {
            const auto* a = reinterpret_cast<const sockaddr_in*>(&address->addr);
            bytes = &a->sin_addr;
            port = ntohs(a->sin_port);
        } else if (address->family == AF_INET6) {
            const auto* a = reinterpret_cast<const sockaddr_in6*>(&address->addr);
            bytes = &a->sin6_addr;
            port = ntohs(a->sin6_port);
        }
        if (!bytes ||
            !inet_ntop(address->family, bytes, buffer.data(), static_cast<unsigned int>(buffer.size()))) {
            item.denied_address = true;
            return CURL_SOCKET_BAD;
        }
        try {
            const bool fixture = item.owner->limits.fixture_loopback_port == port &&
                                 std::string_view(buffer.data()) == "127.0.0.1";
            if (!fixture && !public_address(buffer.data())) {
                item.denied_address = true;
                return CURL_SOCKET_BAD;
            }
        } catch (...) {
            return CURL_SOCKET_BAD;
        }
        return socket(address->family, address->socktype, address->protocol);
    }
    static std::size_t write(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
        auto& item = *static_cast<Item*>(opaque);
        if (size && count > SIZE_MAX / size)
            return 0;
        const auto length = size * count;
        if (item.owner->byte_budget_exhausted ||
            length > item.owner->limits.max_total_bytes - item.owner->total_bytes) {
            item.owner->byte_budget_exhausted = true;
            return 0;
        }
        if (length > item.owner->limits.max_body_bytes - item.response.body.size()) {
            item.too_large = true;
            return 0;
        }
        try {
            item.response.body.append(data, length);
            item.response.bytes += length;
            item.owner->total_bytes += length;
            return length;
        } catch (...) {
            return 0;
        }
    }
    static std::size_t headers(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
        auto& item = *static_cast<Item*>(opaque);
        if (size && count > SIZE_MAX / size)
            return 0;
        const auto length = size * count;
        if (length > 16384)
            return 0;
        try {
            const std::string_view line(data, length);
            const auto split_at = line.find(':');
            if (line.starts_with("HTTP/"))
                item.response.headers = Json::object();
            else if (split_at != line.npos && item.response.headers.size() < 64) {
                auto name = lower(trim(line.substr(0, split_at)));
                if (name == "content-type" || name == "etag" || name == "last-modified" ||
                    name == "location" || name == "retry-after" || name == "cache-control" ||
                    name == "date" || name == "age")
                    item.response.headers[name] = trim(line.substr(split_at + 1));
            }
            return length;
        } catch (...) {
            return 0;
        }
    }
    void prepare(Item& item, Clock::time_point deadline) {
        ++item.response.attempts;
        item.easy = curl_easy_init();
        if (!item.easy)
            throw Error("Could not allocate HTTP request");
        auto* easy = item.easy;
        const auto remaining = std::chrono::duration_cast<Millis>(deadline - Clock::now());
        const auto timeout = std::max<Millis::rep>(1, std::min(remaining, limits.request_timeout).count());
        curl_easy_setopt(easy, CURLOPT_URL, item.current.c_str());
        curl_easy_setopt(easy, CURLOPT_USERAGENT,
                         "DevboxResearch/0.3 (+https://github.com/adybag14-cyber/devbox)");
        curl_easy_setopt(easy, CURLOPT_PROXY, "");
        curl_easy_setopt(easy, CURLOPT_NETRC, CURL_NETRC_IGNORED);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout));
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, std::min(5000L, static_cast<long>(timeout)));
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, item.retries ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        if (const auto ca = tls_ca_bundle()) {
            const auto path = path_text(*ca);
            curl_easy_setopt(easy, CURLOPT_CAINFO, path.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &item);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headers);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &item);
        curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, open_socket);
        curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, &item);
        for (auto it = item.headers.begin(); it != item.headers.end(); ++it) {
            if ((it.key() != "If-None-Match" && it.key() != "If-Modified-Since" &&
                 it.key() != "Cache-Control") ||
                !it.value().is_string())
                continue;
            const auto value = it.value().get<std::string>();
            if (it.key() == "Cache-Control" && value != "no-cache")
                continue;
            if (value.size() < 2048 && value.find_first_of("\r\n") == value.npos)
                item.request_headers =
                    curl_slist_append(item.request_headers, (it.key() + ": " + value).c_str());
        }
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, item.request_headers);
        if (curl_multi_add_handle(multi, easy) != CURLM_OK)
            throw Error("Could not schedule native HTTP request");
    }
};
Transport::Transport(TransportLimits limits) : state_(std::make_unique<State>(std::move(limits))) {}
Transport::~Transport() = default;
std::size_t Transport::downloaded_bytes() const {
    return state_->total_bytes;
}
bool Transport::byte_budget_exhausted() const {
    return state_->byte_budget_exhausted || state_->total_bytes >= state_->limits.max_total_bytes;
}
void Transport::reset_byte_budget() {
    state_->total_bytes = 0;
    state_->byte_budget_exhausted = false;
}
std::vector<Transfer> Transport::get(const std::vector<Request>& requests, Clock::time_point deadline,
                                     const Cancel& cancel) {
    auto& state = *state_;
    std::vector<std::unique_ptr<State::Item>> pending, active;
    pending.reserve(requests.size());
    active.reserve(state.limits.concurrency);
    std::vector<Transfer> results(requests.size());
    auto& backoff = state.backoff;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto item = std::make_unique<State::Item>();
        item->owner = &state;
        item->index = i;
        item->response.url = requests[i].url;
        item->headers = requests[i].headers;
        item->started = Clock::now();
        try {
            item->current = state.checked_url(requests[i].url);
            item->origin = Url::parse(item->current).origin();
            pending.push_back(std::move(item));
        } catch (const std::exception& error) {
            results[i].url = requests[i].url;
            results[i].error = error.what();
        }
    }
    const auto cleanup = [&] {
        for (auto& item : active)
            if (item->easy)
                curl_multi_remove_handle(state.multi, item->easy);
    };
    ScopeExit guard(cleanup);
    const auto finish = [&](State::Item& item) {
        item.response.completed_at = utc_now();
        item.response.completed_unix_ms = unix_millis();
        item.response.final_url = item.current;
        item.response.duration_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<Millis>(Clock::now() - item.started).count());
        results[item.index] = std::move(item.response);
    };
    while (!pending.empty() || !active.empty()) {
        if (cancel)
            cancel->check();
        if (Clock::now() >= deadline || byte_budget_exhausted()) {
            for (auto& items : {&pending, &active})
                for (auto& item : *items) {
                    if (item->easy)
                        curl_multi_remove_handle(state.multi, item->easy);
                    item->response.error = Clock::now() >= deadline ? "WEB_DEADLINE" : "WEB_BYTE_BUDGET";
                    finish(*item);
                }
            pending.clear();
            active.clear();
            break;
        }
        for (auto it = pending.begin(); it != pending.end() && active.size() < state.limits.concurrency;) {
            auto& item = **it;
            if (item.ready_at > Clock::now()) {
                ++it;
                continue;
            }
            const auto peers = std::count_if(active.begin(), active.end(),
                                             [&](const auto& other) { return other->origin == item.origin; });
            if (peers >= static_cast<std::ptrdiff_t>(state.limits.per_origin)) {
                ++it;
                continue;
            }
            if (backoff.contains(item.origin) && backoff[item.origin] > Clock::now()) {
                item.response.error = "WEB_RATE_LIMITED: origin cooldown";
                finish(item);
                it = pending.erase(it);
                continue;
            }
            state.prepare(item, deadline);
            active.push_back(std::move(*it));
            it = pending.erase(it);
        }
        int running = 0;
        if (curl_multi_perform(state.multi, &running) != CURLM_OK)
            throw Error("Native HTTP multi transfer failed");
        int remaining = 0;
        while (auto* message = curl_multi_info_read(state.multi, &remaining)) {
            if (message->msg != CURLMSG_DONE)
                continue;
            auto found = std::find_if(active.begin(), active.end(),
                                      [&](const auto& item) { return item->easy == message->easy_handle; });
            if (found == active.end())
                continue;
            const auto transfer_code = message->data.result;
            auto item = std::move(*found);
            active.erase(found);
            curl_multi_remove_handle(state.multi, item->easy);
            long status = 0;
            curl_easy_getinfo(item->easy, CURLINFO_RESPONSE_CODE, &status);
            item->response.status = static_cast<int>(status);
            if (transfer_code != CURLE_OK)
                item->response.error = state.byte_budget_exhausted ? "WEB_BYTE_BUDGET"
                                       : item->denied_address ? "WEB_PRIVATE_ADDRESS: resolved peer rejected"
                                       : item->too_large      ? "WEB_RESPONSE_TOO_LARGE"
                                                              : curl_easy_strerror(transfer_code);
            curl_easy_cleanup(item->easy);
            item->easy = nullptr;
            if (item->request_headers) {
                curl_slist_free_all(item->request_headers);
                item->request_headers = nullptr;
            }
            if (status == 429 || status == 503) {
                const auto retry = header(item->response.headers, "retry-after");
                long seconds = 60;
                try {
                    seconds = std::clamp(std::stol(retry), 1L, 3600L);
                } catch (...) {
                }
                backoff[item->origin] = Clock::now() + std::chrono::seconds(seconds);
            }
            const bool transient =
                transfer_code == CURLE_COULDNT_RESOLVE_HOST || transfer_code == CURLE_COULDNT_CONNECT ||
                transfer_code == CURLE_OPERATION_TIMEDOUT || transfer_code == CURLE_RECV_ERROR ||
                transfer_code == CURLE_SEND_ERROR || transfer_code == CURLE_GOT_NOTHING ||
                (transfer_code == CURLE_OK && (status == 500 || status == 502 || status == 504));
            if (transient && !state.byte_budget_exhausted && !item->denied_address && !item->too_large &&
                item->retries == 0 && Clock::now() + Millis(750) < deadline &&
                state.total_bytes < state.limits.max_total_bytes) {
                ++item->retries;
                item->ready_at = Clock::now() + Millis(500);
                item->response.error.clear();
                item->response.body.clear();
                item->response.headers = Json::object();
                item->response.status = 0;
                pending.push_back(std::move(item));
                continue;
            }
            if (item->response.error.empty() && redirect_status(item->response.status)) {
                try {
                    if (++item->redirects > 5)
                        throw Error("WEB_REDIRECT_LIMIT");
                    const auto next =
                        state.checked_url(header(item->response.headers, "location"), item->current);
                    const auto origin = Url::parse(next).origin();
                    if (origin != item->origin)
                        item->headers = Json::object();
                    item->current = next;
                    item->origin = origin;
                    item->response.body.clear();
                    item->response.headers = Json::object();
                    pending.push_back(std::move(item));
                    continue;
                } catch (const std::exception& error) {
                    item->response.error = error.what();
                }
            }
            finish(*item);
        }
        if (!active.empty()) {
            int descriptors = 0;
            curl_multi_poll(state.multi, nullptr, 0, 50, &descriptors);
        } else if (!pending.empty()) {
            if (cancel)
                cancel->wait_for(Millis(50));
            else
                std::this_thread::sleep_for(Millis(50));
        }
    }
    return results;
}
} // namespace devbox::web
