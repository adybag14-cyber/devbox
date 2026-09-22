#include "devbox/state_coordinator.hpp"
#include "devbox/contract.hpp"
#include "devbox/resource_budget.hpp"
#include "devbox/result.hpp"
#include "devbox/server.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <thread>
#include <unordered_set>
namespace devbox {
namespace {
constexpr std::size_t frame_limit = 8 * 1024 * 1024;
std::uint64_t monotonic_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<Millis>(Clock::now().time_since_epoch()).count());
}
std::string authenticate(std::string_view key, std::string_view domain, const Json& value) {
    const auto bytes = std::string(domain) + bounded_json_dump(canonical_json(value), frame_limit);
    std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
    unsigned size = 0;
    if (key.size() != 64 ||
        !HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), output.data(), &size))
        throw Error("STATE_IPC_MAC_FAILED");
    return hex(std::span<const std::uint8_t>(output.data(), size));
}
void encryption_key(std::string_view secret, std::string_view direction, std::span<unsigned char, 32> key) {
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
    constexpr std::string_view salt = "devbox-state-ipc-v1";
    std::size_t size = key.size();
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_salt(context.get(), reinterpret_cast<const unsigned char*>(salt.data()),
                                    static_cast<int>(salt.size())) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_key(context.get(), reinterpret_cast<const unsigned char*>(secret.data()),
                                   static_cast<int>(secret.size())) <= 0 ||
        EVP_PKEY_CTX_add1_hkdf_info(context.get(), reinterpret_cast<const unsigned char*>(direction.data()),
                                    static_cast<int>(direction.size())) <= 0 ||
        EVP_PKEY_derive(context.get(), key.data(), &size) <= 0 || size != key.size())
        throw Error("STATE_IPC_KEY_DERIVATION_FAILED");
}
std::string seal(std::string_view secret, std::string_view direction, const Json& value) {
    auto plaintext = bounded_json_dump(value, 5 * 1024 * 1024);
    ScopeExit clear_plain([&] { OPENSSL_cleanse(plaintext.data(), plaintext.size()); });
    std::array<unsigned char, 32> key{};
    ScopeExit clear_key([&] { OPENSSL_cleanse(key.data(), key.size()); });
    encryption_key(secret, direction, key);
    const auto iv = random_bytes(12);
    std::vector<std::uint8_t> bytes(12 + plaintext.size() + 32);
    std::copy(iv.begin(), iv.end(), bytes.begin());
    const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(),
                                                                                 EVP_CIPHER_CTX_free);
    int count = 0, final = 0;
    if (!cipher || EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1 ||
        EVP_EncryptUpdate(cipher.get(), bytes.data() + 12, &count,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(cipher.get(), bytes.data() + 12 + count, &final) != 1)
        throw Error("STATE_IPC_ENCRYPTION_FAILED");
    const auto size = 12 + static_cast<std::size_t>(count + final);
    if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16, bytes.data() + size) != 1)
        throw Error("STATE_IPC_ENCRYPTION_FAILED");
    bytes.resize(size + 16);
    return base64_encode(bytes, true);
}
Json unseal(std::string_view secret, std::string_view direction, std::string_view encoded) {
    if (encoded.size() > 7 * 1024 * 1024)
        throw Error("STATE_IPC_FRAME_LIMIT");
    auto bytes = base64_decode(encoded, true);
    if (bytes.size() < 28 || bytes.size() > 5 * 1024 * 1024 + 28)
        throw Error("STATE_IPC_FRAME_LIMIT");
    std::array<unsigned char, 32> key{};
    ScopeExit clear_key([&] { OPENSSL_cleanse(key.data(), key.size()); });
    encryption_key(secret, direction, key);
    std::string plaintext(bytes.size(), '\0');
    ScopeExit clear_plain([&] { OPENSSL_cleanse(plaintext.data(), plaintext.size()); });
    const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(),
                                                                                 EVP_CIPHER_CTX_free);
    int count = 0, final = 0;
    if (!cipher ||
        EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, key.data(), bytes.data()) != 1 ||
        EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &count,
                          bytes.data() + 12, static_cast<int>(bytes.size() - 28)) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16, bytes.data() + bytes.size() - 16) != 1 ||
        EVP_DecryptFinal_ex(cipher.get(), reinterpret_cast<unsigned char*>(plaintext.data()) + count,
                            &final) != 1)
        throw Error("STATE_IPC_CIPHERTEXT_REJECTED");
    plaintext.resize(static_cast<std::size_t>(count + final));
    std::size_t nodes = 0;
    std::array<std::size_t, 258> widths{};
    return Json::parse(plaintext, [&](int depth, Json::parse_event_t event, Json&) {
        if (depth > 256 || ++nodes > 131072)
            throw Error("STATE_IPC_JSON_LIMIT");
        if (event == Json::parse_event_t::object_start)
            widths[depth + 1] = 0;
        if (event == Json::parse_event_t::key && ++widths[depth] > 4096)
            throw Error("STATE_IPC_JSON_LIMIT");
        return true;
    });
}
Json wire(const StateRecord& value) {
    return Json{{"kind", value.kind},   {"id", value.id},         {"principal", value.principal},
                {"group", value.group}, {"status", value.status}, {"revision", value.revision},
                {"data", value.data}};
}
StateRecord record(const Json& value) {
    return StateRecord{json_string(value, "kind"),
                       json_string(value, "id"),
                       json_string(value, "principal"),
                       json_string(value, "group"),
                       json_string(value, "status"),
                       json_uint(value, "revision"),
                       value.at("data")};
}
std::optional<std::string> optional(const Json& object, const char* name) {
    if (!object.contains(name) || object[name].is_null())
        return {};
    return object[name].get<std::string>();
}
bool running(const Json& endpoint) {
    const auto pid = json_uint(endpoint, "pid");
    return pid && pid <= UINT32_MAX &&
           process_matches_instance(static_cast<std::uint32_t>(pid), json_uint(endpoint, "instance"));
}
Json endpoint_at(const fs::path& root) {
    const auto value = read_json_optional(root / "coordinator.json", 65536);
    if (!value || !value->is_object() || json_uint(*value, "protocol") != 1 ||
        json_uint(*value, "port") < 1 || json_uint(*value, "port") > 65535 ||
        json_string(*value, "key").size() != 64)
        return Json();
    return *value;
}
Json exchange(const Json& endpoint, const Json& payload, Millis timeout, const Cancel& cancel) {
    Json envelope{{"protocol", 1},
                  {"nonce", uuid()},
                  {"issued_steady_ms", monotonic_ms()},
                  {"generation", endpoint.at("generation")},
                  {"sealed", seal(json_string(endpoint, "key"), "request", payload)}};
    const auto nonce = json_string(envelope, "nonce");
    envelope["mac"] = authenticate(json_string(endpoint, "key"), "devbox-state-request-v1:", envelope);
    Json request{{"jsonrpc", "2.0"},
                 {"id", nonce},
                 {"method", "tools/call"},
                 {"params", {{"name", "devbox_internal_state"}, {"arguments", envelope}}}};
    const auto response =
        http_request("POST", "http://127.0.0.1:" + std::to_string(json_uint(endpoint, "port")) + "/mcp",
                     bounded_json_dump(request, frame_limit),
                     Json{{"content-type", "application/json"},
                          {"accept", "application/json"},
                          {"user-agent", "devbox-state-ipc/1"}},
                     timeout, frame_limit, cancel, true);
    if (response.status != 200)
        throw Error("STATE_IPC_UNAVAILABLE");
    const auto parsed = Json::parse(response.body);
    if (!parsed.contains("result") || !parsed["result"].contains("structuredContent") ||
        !parsed["result"]["structuredContent"].contains("data"))
        throw Error("STATE_IPC_SERVER_PROOF_FAILED");
    auto reply = parsed.at("result").at("structuredContent").at("data");
    const auto mac = json_string(reply, "mac");
    reply.erase("mac");
    if (json_string(reply, "nonce") != nonce || reply.at("generation") != endpoint.at("generation") ||
        !constant_time_equal(mac,
                             authenticate(json_string(endpoint, "key"), "devbox-state-response-v1:", reply)))
        throw Error("STATE_IPC_SERVER_PROOF_FAILED");
    const auto data = unseal(json_string(endpoint, "key"), "response", json_string(reply, "sealed"));
    if (!json_bool(data, "ok"))
        throw Error(json_string(data, "error", "STATE_IPC_OPERATION_FAILED"));
    return data.at("result");
}
class Coordinator final : public McpBackend {
  public:
    std::atomic_bool stop{false};

  private:
    std::shared_ptr<StateStore> store_;
    std::string key_;
    std::unordered_set<std::string> nonces_;
    std::deque<std::pair<std::string, std::uint64_t>> expirations_;
    // Destroy/join the actor before the state it references.
    WorkPool actor_{1, 32};
    Json dispatch(const Json& payload) {
        const auto op = json_string(payload, "op");
        if (op == "diagnostics")
            return store_->diagnostics();
        if (op == "private_snapshot")
            return store_->private_snapshot(path_from_utf8(json_string(payload, "destination")));
        if (op == "get") {
            const auto value = store_->get(json_string(payload, "kind"), json_string(payload, "id"));
            return value ? wire(*value) : Json();
        }
        if (op == "count")
            return store_->count(json_string(payload, "kind"), optional(payload, "principal"),
                                 optional(payload, "status"));
        if (op == "count_matching")
            return store_->count_matching(
                StateCountQuery{json_string(payload, "kind"), optional(payload, "principal"),
                                optional(payload, "group"), json_strings(payload, "statuses"),
                                static_cast<std::size_t>(json_uint(payload, "limit", 256))});
        if (op == "list") {
            StateQuery query{
                json_string(payload, "kind"), optional(payload, "principal"),
                optional(payload, "group"),   optional(payload, "status"),
                optional(payload, "after"),   static_cast<std::size_t>(json_uint(payload, "limit", 50))};
            const auto page = store_->list(query);
            Json records = Json::array();
            for (const auto& value : page.records)
                records.push_back(wire(value));
            return Json{{"records", records}, {"next", page.next ? Json(*page.next) : Json()}};
        }
        if (op == "events") {
            Json values = Json::array();
            for (const auto& event : store_->events(json_string(payload, "run"), json_uint(payload, "after"),
                                                    json_uint(payload, "limit", 50)))
                values.push_back(Json{{"run", event.run},
                                      {"type", event.type},
                                      {"sequence", event.sequence},
                                      {"data", event.data}});
            return values;
        }
        if (op == "import") {
            if (!payload.at("records").is_array() || payload["records"].size() > 256)
                throw Error("STATE_BATCH_LIMIT");
            std::vector<StateRecord> values;
            for (const auto& value : payload["records"])
                values.push_back(record(value));
            store_->import_records(values);
            return Json{{"imported", true}};
        }
        if (op == "apply") {
            if (!payload.at("mutations").is_array() || !payload.at("events").is_array() ||
                payload["mutations"].size() > 256 || payload["events"].size() > 256)
                throw Error("STATE_BATCH_LIMIT");
            std::vector<StateMutation> mutations;
            std::vector<StateEvent> events;
            for (const auto& value : payload["mutations"])
                mutations.push_back(StateMutation{record(value), json_uint(value, "expected_revision")});
            for (const auto& value : payload["events"])
                events.push_back(StateEvent{json_string(value, "run"), json_string(value, "type"),
                                            json_uint(value, "sequence"), value.at("data")});
            return Json{
                {"replayed", store_->apply_once(json_string(payload, "batch_id"), mutations, events)}};
        }
        if (op == "stop") {
            stop = true;
            return Json{{"accepted", true}};
        }
        throw Error("STATE_IPC_UNKNOWN_OPERATION");
    }
    Json handle(Json request) {
        const auto mac = json_string(request, "mac");
        request.erase("mac");
        if (!constant_time_equal(mac, authenticate(key_, "devbox-state-request-v1:", request)))
            return result_error("STATE_IPC_AUTHENTICATION_FAILED");
        const auto now = monotonic_ms(), issued = json_uint(request, "issued_steady_ms");
        const auto nonce = json_string(request, "nonce");
        Json reply{{"nonce", nonce}, {"generation", store_->generation()}};
        Json data{{"ok", false}};
        try {
            while (!expirations_.empty() && expirations_.front().second < now) {
                nonces_.erase(expirations_.front().first);
                expirations_.pop_front();
            }
            if (json_uint(request, "protocol") != 1 || nonce.size() != 36 || issued > now + 1000 ||
                now > issued + 30000 || nonces_.size() >= 16384 || !nonces_.insert(nonce).second)
                throw Error("STATE_IPC_REPLAY_OR_EXPIRY");
            expirations_.emplace_back(nonce, now + 31000);
            if (json_uint(request, "generation") != store_->generation())
                throw Error("STATE_WRITER_FENCED");
            data["result"] = dispatch(unseal(key_, "request", json_string(request, "sealed")));
            data["ok"] = true;
        } catch (const std::exception& error) {
            const std::string message = error.what();
            data["error"] = message.starts_with("STATE_") ? message : "STATE_IPC_OPERATION_FAILED";
        }
        reply["sealed"] = seal(key_, "response", data);
        reply["mac"] = authenticate(key_, "devbox-state-response-v1:", reply);
        return result_explicit("Authenticated state coordinator response.", std::move(reply),
                               "State coordinator response.");
    }

  public:
    Coordinator(std::shared_ptr<StateStore> store, std::string key)
        : store_(std::move(store)), key_(std::move(key)) {}
    Json server_info() const override {
        return Json{{"name", "Devbox private state coordinator"}, {"version", "1"}};
    }
    Json list_tools(std::string_view) const override {
        return Json::array({Json{{"name", "devbox_internal_state"}, {"inputSchema", {{"type", "object"}}}}});
    }
    bool has_tool(std::string_view name) const override {
        return name == "devbox_internal_state";
    }
    bool ready() const override {
        return !stop;
    }
    asio::awaitable<Json> metadata(const HttpRequest&) override {
        co_return server_info();
    }
    void observe_http(const HttpRequest&, int, std::uint64_t, Millis, bool) override {}
    asio::awaitable<Json> call_tool(std::string, Json args, Cancel cancel) override {
        auto task =
            actor_.run([this, args = std::move(args)]() mutable { return handle(std::move(args)); }, cancel);
        co_return co_await std::move(task);
    }
};
class StateClient final : public StateStore {
    fs::path root_;
    StateClientOptions options_;
    mutable std::mutex endpoint_mutex_;
    mutable Json endpoint_;
    std::atomic_bool writable_{true};
    Json ensure() const {
        std::lock_guard lock(endpoint_mutex_);
        if (!endpoint_.is_null() && running(endpoint_))
            return endpoint_;
        endpoint_ = endpoint_at(root_);
        if (!endpoint_.is_null() && running(endpoint_))
            return endpoint_;
        if (!options_.start_if_absent)
            throw Error("STATE_COORDINATOR_ABSENT");
        FileLock start(root_ / ".coordinator-start.lock", Millis(5000), options_.cancel, true);
        endpoint_ = endpoint_at(root_);
        if (!endpoint_.is_null() && running(endpoint_))
            return endpoint_;
        const auto executable = options_.executable.empty() ? executable_path() : options_.executable;
        if (!executable.is_absolute())
            throw Error("STATE_COORDINATOR_EXECUTABLE_MUST_BE_ABSOLUTE");
        std::optional<std::uint64_t> instance;
        const auto pid =
            spawn_detached(executable, {"--state-coordinator", path_text(root_)}, root_, {}, &instance);
        const auto deadline = Clock::now() + Millis(10000);
        while (Clock::now() < deadline) {
            if (options_.cancel)
                options_.cancel->check();
            auto candidate = endpoint_at(root_);
            if (!candidate.is_null() && json_uint(candidate, "pid") == pid && running(candidate) &&
                (!instance || json_uint(candidate, "instance") == *instance)) {
                endpoint_ = std::move(candidate);
                return endpoint_;
            }
            if (instance && !process_matches_instance(pid, instance))
                throw Error("STATE_COORDINATOR_START_FAILED");
            std::this_thread::sleep_for(Millis(20));
        }
        throw Error("STATE_COORDINATOR_START_TIMEOUT");
    }
    Json rpc(const Json& request) const {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto endpoint = ensure();
            try {
                return exchange(endpoint, request, options_.timeout, options_.cancel);
            } catch (const Cancelled&) {
                throw;
            } catch (const Error& error) {
                const std::string message = error.what();
                if (!message.starts_with("HTTP request failed") && !message.starts_with("STATE_IPC_") &&
                    message != "STATE_WRITER_FENCED")
                    throw;
                if (attempt)
                    throw Error("STATE_IPC_OUTCOME_UNCONFIRMED: retain the same batch or operation identity");
                std::lock_guard lock(endpoint_mutex_);
                endpoint_ = nullptr;
            }
        }
        throw Error("STATE_IPC_UNAVAILABLE");
    }

  public:
    StateClient(fs::path root, StateClientOptions options)
        : root_(std::move(root)), options_(std::move(options)) {
        ensure_directory(root_.parent_path());
        ensure_private_state_directory(root_);
        (void)ensure();
    }
    std::optional<StateRecord> get(std::string_view kind, std::string_view id) const override {
        const auto result = rpc(Json{{"op", "get"}, {"kind", kind}, {"id", id}});
        return result.is_null() ? std::nullopt : std::optional(record(result));
    }
    StatePage list(const StateQuery& query) const override {
        Json request{{"op", "list"}, {"kind", query.kind}, {"limit", query.limit}};
        for (const auto& [key, value] : {std::pair{"principal", query.principal},
                                         {"group", query.group},
                                         {"status", query.status},
                                         {"after", query.after}})
            if (value)
                request[key] = *value;
        const auto result = rpc(request);
        StatePage page;
        page.next = optional(result, "next");
        for (const auto& value : result.at("records"))
            page.records.push_back(record(value));
        return page;
    }
    std::uint64_t count(std::string_view kind, const std::optional<std::string>& principal,
                        const std::optional<std::string>& status) const override {
        Json request{{"op", "count"}, {"kind", kind}};
        if (principal)
            request["principal"] = *principal;
        if (status)
            request["status"] = *status;
        return rpc(request).get<std::uint64_t>();
    }
    std::uint64_t count_matching(const StateCountQuery& query) const override {
        Json request{{"op", "count_matching"},
                     {"kind", query.kind},
                     {"statuses", query.statuses},
                     {"limit", query.limit}};
        if (query.principal)
            request["principal"] = *query.principal;
        if (query.group)
            request["group"] = *query.group;
        return rpc(request).get<std::uint64_t>();
    }
    void apply(std::span<const StateMutation> mutations, std::span<const StateEvent> events) override {
        (void)apply_once(uuid(), mutations, events);
    }
    bool apply_once(std::string_view batch, std::span<const StateMutation> mutations,
                    std::span<const StateEvent> events) override {
        if (!writable_)
            throw Error("STATE_WRITER_FENCED");
        Json request{
            {"op", "apply"}, {"batch_id", batch}, {"mutations", Json::array()}, {"events", Json::array()}};
        for (const auto& change : mutations) {
            auto value = wire(change.record);
            value["expected_revision"] = change.expected_revision;
            request["mutations"].push_back(std::move(value));
        }
        for (const auto& event : events)
            request["events"].push_back(Json{{"run", event.run},
                                             {"type", event.type},
                                             {"sequence", event.sequence},
                                             {"data", event.data}});
        return json_bool(rpc(request), "replayed");
    }
    void import_records(std::span<const StateRecord> values) override {
        if (!writable_)
            throw Error("STATE_WRITER_FENCED");
        Json request{{"op", "import"}, {"records", Json::array()}};
        for (const auto& value : values)
            request["records"].push_back(wire(value));
        (void)rpc(request);
    }
    std::vector<StateEvent> events(std::string_view run, std::uint64_t after,
                                   std::size_t limit) const override {
        std::vector<StateEvent> result;
        for (const auto& value :
             rpc(Json{{"op", "events"}, {"run", run}, {"after", after}, {"limit", limit}}))
            result.push_back(StateEvent{json_string(value, "run"), json_string(value, "type"),
                                        json_uint(value, "sequence"), value.at("data")});
        return result;
    }
    std::uint64_t generation() const override {
        return json_uint(ensure(), "generation");
    }
    void release_writer() override {
        writable_ = false;
    }
    Json diagnostics() const override {
        return rpc(Json{{"op", "diagnostics"}});
    }
    Json private_snapshot(const fs::path& destination) override {
        return rpc(Json{{"op", "private_snapshot"}, {"destination", path_text(destination)}});
    }
};
} // namespace
std::shared_ptr<StateStore> open_coordinated_state(const fs::path& directory, StateClientOptions options) {
    return std::make_shared<StateClient>(directory, std::move(options));
}
int run_state_coordinator(const fs::path& root, const std::function<bool()>& stop_requested) {
    auto store = open_state_store(root);
    const auto key = hex(random_bytes(32));
    auto backend = std::make_shared<Coordinator>(store, key);
    auto config = std::make_shared<Config>();
    config->host = "127.0.0.1";
    config->port = 0;
    config->project_root = root;
    config->platform = Platform::detect();
    config->internal_json_depth = 256;
    config->mcp_response_deadline_ms = 10000;
    HttpServer server(config, backend);
    const auto port = server.start();
    write_json_atomic(root / "coordinator.json", Json{{"protocol", 1},
                                                      {"pid", process_id()},
                                                      {"instance", *process_instance(process_id())},
                                                      {"port", port},
                                                      {"generation", store->generation()},
                                                      {"key", key},
                                                      {"binary_sha256", build_snapshot()["binarySha256"]}});
    while (!stop_requested() && !backend->stop)
        std::this_thread::sleep_for(Millis(50));
    server.stop();
    return 0;
}
bool stop_state_coordinator(const fs::path& root, Millis wait) {
    const auto endpoint = endpoint_at(root);
    if (endpoint.is_null() || !running(endpoint))
        return true;
    try {
        (void)exchange(endpoint, Json{{"op", "stop"}}, Millis(1000), {});
    } catch (...) {
    }
    const auto deadline = Clock::now() + wait;
    while (running(endpoint) && Clock::now() < deadline)
        std::this_thread::sleep_for(Millis(10));
    return !running(endpoint);
}
} // namespace devbox
