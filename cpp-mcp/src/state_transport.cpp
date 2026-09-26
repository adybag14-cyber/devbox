#include "devbox/state_transport.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <array>
#include <curl/curl.h>
#include <limits>
#include <utility>
namespace devbox {
namespace {
constexpr std::size_t frame_limit = 8 * 1024 * 1024;
struct CurlDelete {
    void operator()(CURL* value) const noexcept {
        curl_easy_cleanup(value);
    }
};
using Curl = std::unique_ptr<CURL, CurlDelete>;
void checked(CURLcode code) {
    if (code != CURLE_OK)
        throw Error(std::string("HTTP request failed: ") + curl_easy_strerror(code));
}
} // namespace
struct StateHttpTransport::Pool {
    mutable std::mutex mutex;
    std::array<Curl, 4> idle;
    Peer peer;
    std::uint64_t epoch = 0, transfers = 0, connections = 0;
    const bool reuse;
    explicit Pool(bool enabled) : reuse(enabled) {}
};
struct StateHttpTransport::Attempt {
    std::shared_ptr<Pool> pool;
    Curl handle;
    Peer peer;
    std::uint64_t epoch = 0;
    HttpResult result;
    Cancel cancel;
    std::size_t max_bytes = 0, header_bytes = 0;
    bool oversized = false, accepted = false;
    std::exception_ptr callback_error;
    ~Attempt() {
        if (!handle)
            return;
        // Reset before borrowed body/header/callback storage can expire. libcurl
        // retains the socket, not request options. Cookies/netrc are never enabled.
        curl_easy_reset(handle.get());
        if (!accepted || !pool->reuse || result.status != 200)
            return;
        std::lock_guard lock(pool->mutex);
        if (epoch != pool->epoch || !(peer == pool->peer))
            return; // A retry/peer change fenced this in-flight connection.
        for (auto& slot : pool->idle)
            if (!slot) {
                slot = std::move(handle);
                return;
            }
        // No capacity waiting or dropped request: excess concurrent transfers are
        // ephemeral and close here. At most four idle connections per client.
    }
    static std::size_t write(char* data, std::size_t a, std::size_t b, void* context) noexcept {
        auto& self = *static_cast<Attempt*>(context);
        try {
            if (b && a > std::numeric_limits<std::size_t>::max() / b)
                return 0;
            const auto size = a * b;
            if (size > self.max_bytes - self.result.body.size()) {
                self.oversized = true;
                return 0;
            }
            self.result.body.append(data, size);
            return size;
        } catch (...) {
            self.callback_error = std::current_exception();
            return 0;
        }
    }
    static std::size_t header(char*, std::size_t a, std::size_t b, void* context) noexcept {
        auto& self = *static_cast<Attempt*>(context);
        if (b && a > std::numeric_limits<std::size_t>::max() / b)
            return 0;
        const auto size = a * b;
        if (size > 32768 - self.header_bytes) {
            self.oversized = true;
            return 0;
        }
        self.header_bytes += size;
        return size;
    }
    static int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        const auto& self = *static_cast<Attempt*>(context);
        return self.cancel && self.cancel->cancelled() ? 1 : 0;
    }
};
StateHttpTransport::Transfer::Transfer(std::unique_ptr<Attempt> attempt) : attempt_(std::move(attempt)) {}
StateHttpTransport::Transfer::~Transfer() = default;
StateHttpTransport::Transfer::Transfer(Transfer&&) noexcept = default;
StateHttpTransport::Transfer& StateHttpTransport::Transfer::operator=(Transfer&&) noexcept = default;
const HttpResult& StateHttpTransport::Transfer::response() const {
    return attempt_->result;
}
void StateHttpTransport::Transfer::verified() {
    attempt_->accepted = true;
}
StateHttpTransport::StateHttpTransport(bool reuse) : pool_(std::make_shared<Pool>(reuse)) {}
StateHttpTransport::~StateHttpTransport() = default;
StateHttpTransport::Transfer StateHttpTransport::post(Peer peer, std::string_view body, Millis timeout,
                                                      std::size_t max_bytes, const Cancel& cancel) const {
    if (cancel)
        cancel->check();
    if (!peer.port || !peer.generation || !peer.pid || !peer.instance)
        throw Error("STATE_IPC_INVALID_PEER");
    if (body.size() > frame_limit || !max_bytes || max_bytes > frame_limit)
        throw Error("STATE_IPC_FRAME_LIMIT");
    static const bool initialized = [] {
        checked(curl_global_init(CURL_GLOBAL_DEFAULT));
        return true;
    }();
    (void)initialized;
    auto attempt = std::make_unique<Attempt>();
    attempt->pool = pool_;
    attempt->peer = peer;
    attempt->max_bytes = max_bytes;
    attempt->cancel = cancel;
    // Destroy retired handles outside the pool lock. No I/O or capacity wait is
    // performed while serializing the bounded idle cache.
    std::array<Curl, 4> retired;
    {
        std::lock_guard lock(pool_->mutex);
        if (!(pool_->peer == peer)) {
            retired.swap(pool_->idle);
            pool_->peer = peer;
            ++pool_->epoch;
        }
        attempt->epoch = pool_->epoch;
        if (pool_->reuse)
            for (auto& slot : pool_->idle)
                if (slot) {
                    attempt->handle = std::move(slot);
                    break;
                }
    }
    if (!attempt->handle)
        attempt->handle.reset(curl_easy_init());
    if (!attempt->handle)
        throw Error("HTTP allocation failed");
    auto* curl = attempt->handle.get();
    const auto url = "http://127.0.0.1:" + std::to_string(peer.port) + "/mcp";
    curl_slist* headers = nullptr;
    ScopeExit free_headers([&] { curl_slist_free_all(headers); });
    ScopeExit reset_options([&] { curl_easy_reset(curl); });
    for (const auto* field :
         {"Content-Type: application/json", "Accept: application/json", "User-Agent: devbox-state-ipc/1"}) {
        auto* next = curl_slist_append(headers, field);
        if (!next)
            throw Error("HTTP allocation failed");
        headers = next;
    }
    const auto timeout_ms = static_cast<long>(std::clamp<std::int64_t>(timeout.count(), 1, 2147483647));
    checked(curl_easy_setopt(curl, CURLOPT_URL, url.c_str()));
    checked(curl_easy_setopt(curl, CURLOPT_PROXY, ""));
    checked(curl_easy_setopt(curl, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED)));
    checked(curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE)));
    checked(curl_easy_setopt(curl, CURLOPT_POST, 1L));
    checked(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data()));
    checked(curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())));
    checked(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers));
    checked(curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1)));
    checked(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L));
    checked(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms));
    checked(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(5000L, timeout_ms)));
    checked(curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L));
    checked(curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L));
    checked(curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http"));
    checked(curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L));
    checked(curl_easy_setopt(curl, CURLOPT_MAXAGE_CONN, 10L));
    checked(curl_easy_setopt(curl, CURLOPT_MAXLIFETIME_CONN, 60L));
    checked(curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, pool_->reuse ? 0L : 1L));
    checked(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Attempt::write));
    checked(curl_easy_setopt(curl, CURLOPT_WRITEDATA, attempt.get()));
    checked(curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Attempt::header));
    checked(curl_easy_setopt(curl, CURLOPT_HEADERDATA, attempt.get()));
    checked(curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L));
    checked(curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Attempt::progress));
    checked(curl_easy_setopt(curl, CURLOPT_XFERINFODATA, attempt.get()));
    const auto code = curl_easy_perform(curl);
    long connections = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connections);
    {
        std::lock_guard lock(pool_->mutex);
        ++pool_->transfers;
        if (connections > 0)
            pool_->connections += static_cast<std::uint64_t>(connections);
    }
    if (attempt->callback_error)
        std::rethrow_exception(attempt->callback_error);
    if (attempt->oversized)
        throw Error("STATE_IPC_FRAME_LIMIT");
    if (cancel)
        cancel->check();
    checked(code);
    long status = 0;
    checked(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status));
    attempt->result.status = static_cast<int>(status);
    // Keep options pointing to caller-owned buffers only until this call returns.
    curl_easy_reset(curl);
    return Transfer(std::move(attempt));
}
void StateHttpTransport::discard_idle() const {
    std::array<Curl, 4> retired;
    {
        std::lock_guard lock(pool_->mutex);
        retired.swap(pool_->idle);
        ++pool_->epoch;
    }
}
StateHttpTransport::Statistics StateHttpTransport::statistics() const {
    std::lock_guard lock(pool_->mutex);
    return Statistics{pool_->transfers, pool_->connections,
                      static_cast<std::size_t>(std::count_if(pool_->idle.begin(), pool_->idle.end(),
                                                             [](const auto& handle) { return !!handle; }))};
}
} // namespace devbox
