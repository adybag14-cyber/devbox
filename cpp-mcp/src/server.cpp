#include "devbox/server.hpp"
#include "devbox/result.hpp"
#include <array>
#include <boost/beast.hpp>
#include <iostream>
#include <libdeflate.h>
#include <set>
#include <unordered_map>
#include <zlib.h>

namespace devbox {
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = asio::ip::tcp;
namespace {
Json rpc_error(const Json& id, int code, const std::string& message, const Json& data = nullptr) {
    Json error{{"code", code}, {"message", message}};
    if (!data.is_null())
        error["data"] = data;
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}
Json rpc_result(const Json& id, Json result) {
    Json response{{"jsonrpc", "2.0"}, {"id", id}};
    response["result"] = std::move(result);
    return response;
}
int media_quality(std::string_view accept, std::string_view type, int missing = 0) {
    int best = missing;
    for (const auto& range : split(accept, ',')) {
        const auto parts = split(range, ';');
        if (parts.empty() || lower(trim(parts.front())) != type)
            continue;
        int quality = 1000;
        for (std::size_t index = 1; index < parts.size(); ++index) {
            const auto parameter = lower(trim(parts[index]));
            const auto equals = parameter.find('=');
            if (equals == parameter.npos || trim(parameter.substr(0, equals)) != "q")
                continue;
            const auto value = trim(parameter.substr(equals + 1));
            quality = 0;
            if (value.empty() || (value[0] != '0' && value[0] != '1') || value.size() > 5 ||
                (value.size() > 1 && value[1] != '.'))
                continue;
            int parsed = value[0] == '1' ? 1000 : 0;
            int place = 100;
            bool valid = true;
            for (std::size_t digit = 2; digit < value.size(); ++digit) {
                if (value[digit] < '0' || value[digit] > '9' || (value[0] == '1' && value[digit] != '0')) {
                    valid = false;
                    break;
                }
                parsed += (value[digit] - '0') * place;
                place /= 10;
            }
            if (valid)
                quality = parsed;
        }
        best = std::max(best, quality);
    }
    return best;
}
bool prefers_json(std::string_view accept) {
    const auto json = media_quality(accept, "application/json");
    return json > 0 && json >= media_quality(accept, "text/event-stream");
}
std::string_view preferred_encoding(std::string_view accept) {
    const auto quality = [accept](std::string_view coding) {
        const auto specified = media_quality(accept, coding, -1);
        return specified < 0 ? media_quality(accept, "*") : specified;
    };
    const auto gzip = quality("gzip"), deflate = quality("deflate");
    const auto identity = media_quality(accept, "identity");
    if (deflate > 0 && deflate >= gzip && deflate >= identity)
        return "deflate";
    if (gzip > 0 && gzip >= identity)
        return "gzip";
    return {};
}
std::optional<std::string> compress_bounded(std::string_view body, std::string_view coding,
                                            const Cancel& cancel) {
    cancel->check();
    const std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)> compressor(
        libdeflate_alloc_compressor(1), libdeflate_free_compressor);
    if (!compressor)
        return std::nullopt;
    const auto encode = coding == "gzip" ? libdeflate_gzip_compress : libdeflate_zlib_compress;
    std::array<char, 4 * 1024> probe;
    if (!encode(compressor.get(), body.data(), 8 * 1024, probe.data(), probe.size()))
        return std::nullopt;
    cancel->check();
    std::string output;
    const auto limit = body.size() / 2;
    output.resize_and_overwrite(limit, [&](char* buffer, std::size_t) {
        return encode(compressor.get(), body.data(), body.size(), buffer, limit);
    });
    cancel->check();
    if (output.empty())
        return std::nullopt;
    return output;
}
asio::awaitable<std::optional<std::string>> compress_json(std::string_view body, std::string_view coding,
                                                          const Cancel& cancel) {
    // Compression is optional and bounded. Larger responses retain their original
    // representation, and the stream owns no cross-request dictionary or state.
    if (body.size() < 128 * 1024 || body.size() > 16 * 1024 * 1024)
        co_return std::nullopt;
    // Whole-buffer compression is limited to one MiB. Larger responses use
    // incremental compression so cancellation and other I/O can make progress.
    if (body.size() <= 1024 * 1024)
        co_return compress_bounded(body, coding, cancel);
    z_stream state{};
    const auto bits = MAX_WBITS + (coding == "gzip" ? 16 : 0);
    if (deflateInit2(&state, Z_BEST_SPEED, Z_DEFLATED, bits, 5, Z_DEFAULT_STRATEGY) != Z_OK)
        co_return std::nullopt;
    struct End {
        z_stream* state;
        ~End() {
            deflateEnd(state);
        }
    } end{&state};
    // Keep large scratch storage out of the coroutine frame: the frame is
    // allocated even when the bounded path above returns before reaching here.
    auto block = std::make_unique_for_overwrite<std::array<char, 64 * 1024>>();
    std::string output;
    output.reserve(block->size());
    const auto limit = body.size() / 2;
    auto slice_started = Clock::now();
    for (std::size_t offset = 0; offset < body.size();) {
        cancel->check();
        // Sample a small prefix before spending CPU on a low-compressibility
        // payload. Sync flush exposes its actual encoded size without resetting
        // the dictionary used by the rest of this response.
        const auto size = std::min<std::size_t>(offset ? 64 * 1024 : 8 * 1024, body.size() - offset);
        state.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(body.data() + offset));
        state.avail_in = static_cast<uInt>(size);
        offset += size;
        int result;
        do {
            state.next_out = reinterpret_cast<Bytef*>(block->data());
            state.avail_out = static_cast<uInt>(block->size());
            result = deflate(&state, offset == body.size() ? Z_FINISH
                                     : offset == 8 * 1024  ? Z_SYNC_FLUSH
                                                           : Z_NO_FLUSH);
            if ((result != Z_OK && result != Z_STREAM_END) || state.total_out >= limit)
                co_return std::nullopt;
            output.append(block->data(), block->size() - state.avail_out);
        } while (state.avail_in || (offset == body.size() && result != Z_STREAM_END));
        if (output.size() > offset / 2)
            co_return std::nullopt;
        // Bound uninterrupted CPU work so other requests and disconnects run.
        if (offset < body.size() && Clock::now() - slice_started >= Millis(1)) {
            co_await asio::post(asio::use_awaitable);
            slice_started = Clock::now();
        }
    }
    cancel->check();
    co_return output;
}
bool supported_protocol(std::string_view value) {
    return value == "2024-11-05" || value == "2025-03-26" || value == "2025-06-18" || value == "2025-11-25" ||
           value == "2026-07-28";
}
Json supported_protocols() {
    return Json::array({"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"});
}
bool modern_protocol(const HttpRequest& request) {
    return json_string(request.headers, "mcp-protocol-version") >= "2026-07-28";
}
Json capabilities() {
    return Json{{"logging", Json::object()}, {"tools", {{"listChanged", true}}}};
}
// The frozen rmcp 3.1.4 server is the compatibility authority. Modern requests
// carry their own lifecycle metadata; legacy SDKs retain the initialize flow.
std::optional<HttpReply> validate_protocol_request(const HttpRequest& request, const Json& body) {
    const auto id = body.value("id", Json(nullptr));
    const auto method = json_string(body, "method");
    const auto header = json_string(request.headers, "mcp-protocol-version");
    const auto params = body.value("params", Json::object());
    const auto meta = params.is_object() ? params.value("_meta", Json::object()) : Json::object();
    const auto version_key = "io.modelcontextprotocol/protocolVersion";
    const auto caps_key = "io.modelcontextprotocol/clientCapabilities";
    const auto meta_version = json_string(meta, version_key);
    const auto fail = [&](int code, const std::string& message, const Json& data = nullptr) {
        return HttpReply::json(400, rpc_error(id, code, message, data));
    };
    if (!header.empty() && !supported_protocol(header))
        return fail(-32022, "Unsupported protocol version",
                    Json{{"requested", header}, {"supported", supported_protocols()}});
    if (method == "initialize") {
        const auto requested = json_string(params, "protocolVersion");
        if (!header.empty() && header != requested)
            return fail(-32600, "Invalid Request: MCP-Protocol-Version header (" + header +
                                    ") does not match initialize params.protocolVersion (" + requested + ")");
        return {};
    }
    if (!body.contains("id"))
        return {};
    if (!meta_version.empty()) {
        if (header.empty())
            return fail(-32020, "request _meta protocolVersion requires MCP-Protocol-Version header");
        if (header != meta_version)
            return fail(-32020, "MCP-Protocol-Version header (" + header +
                                    ") does not match request _meta protocolVersion (" + meta_version + ")");
    }
    if (modern_protocol(request) || method == "server/discover") {
        std::string missing;
        if (meta_version.empty())
            missing = version_key;
        if (!meta.is_object() || !meta.contains(caps_key) || !meta[caps_key].is_object()) {
            if (!missing.empty())
                missing += ", ";
            missing += caps_key;
        }
        if (!missing.empty())
            return fail(-32602, std::string(meta_version.empty() ? "Invalid params: " : "") +
                                    "request _meta is missing or has malformed required fields: " + missing);
    }
    if (!modern_protocol(request))
        return {};
    const auto mirrored_method = json_string(request.headers, "mcp-method");
    if (mirrored_method.empty())
        return fail(-32020, "missing required Mcp-Method header");
    if (mirrored_method != method)
        return fail(-32020, "Mcp-Method header `" + mirrored_method + "` does not match body method `" +
                                method + "`");
    if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
        const auto field = method == "resources/read" ? "uri" : "name";
        const auto expected = json_string(params, field);
        if (!request.headers.contains("mcp-name"))
            return fail(-32020, "missing required Mcp-Name header for `" + method + "`");
        auto mirrored = json_string(request.headers, "mcp-name");
        if (mirrored.starts_with("=?base64?") && mirrored.ends_with("?=")) {
            try {
                const auto bytes = base64_decode(std::string_view(mirrored).substr(9, mirrored.size() - 11));
                mirrored.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                // dump() validates decoded UTF-8; malformed header bytes never reach dispatch.
                (void)Json(mirrored).dump();
            } catch (...) {
                return fail(-32020, "Mcp-Name header is not valid Base64");
            }
        }
        if (mirrored != expected)
            return fail(-32020,
                        "Mcp-Name header `" + mirrored + "` does not match body value `" + expected + "`");
    }
    return {};
}
Json bounded_json(std::string_view body) {
    return Json::parse(body, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 128)
            throw Error("JSON nesting exceeds the limit");
        return true;
    });
}
HttpReply oauth_failure(const OAuthFailure& failure) {
    auto reply = HttpReply::json(failure.status, failure.response());
    reply.headers["cache-control"] = "no-store";
    return reply;
}
HttpReply oauth_route(OAuthService& oauth, const HttpRequest& request, const Cancel& cancel) {
    const auto parameters = query_parameters(request.method == "GET" ? request.query : request.body);
    try {
        Json value;
        int status = 200;
        if (request.path == "/register") {
            value = oauth.register_client(bounded_json(request.body));
            status = 201;
        } else if (request.path == "/authorize") {
            std::optional<std::string> assertion, email;
            if (request.headers.contains("cf-access-jwt-assertion"))
                assertion = json_string(request.headers, "cf-access-jwt-assertion");
            if (request.headers.contains("cf-access-authenticated-user-email"))
                email = json_string(request.headers, "cf-access-authenticated-user-email");
            return HttpReply{302, "",
                             Json{{"location", oauth.authorize(parameters, assertion, email, cancel)},
                                  {"cache-control", "no-store"}}};
        } else if (request.path == "/token")
            value = oauth.exchange_token(parameters);
        else {
            oauth.revoke(parameters);
            value = Json::object();
        }
        auto reply = HttpReply::json(status, value);
        reply.headers["cache-control"] = "no-store";
        return reply;
    } catch (const OAuthFailure& e) {
        if (request.path == "/authorize" && e.safe_redirect_uri) {
            Json pairs{{"error", e.code}, {"error_description", e.what()}};
            if (parameters.contains("state"))
                pairs["state"] = parameters["state"];
            return HttpReply{
                302, "",
                Json{{"location", append_query(*e.safe_redirect_uri, pairs)}, {"cache-control", "no-store"}}};
        }
        return oauth_failure(e);
    } catch (const Json::exception&) {
        return oauth_failure(OAuthFailure("invalid_request", "Invalid JSON body"));
    }
}
} // namespace
struct HttpServer::Impl : std::enable_shared_from_this<HttpServer::Impl> {
    struct Session;
    std::shared_ptr<const Config> config;
    std::shared_ptr<McpBackend> backend;
    Gateway gateway;
    std::unique_ptr<OAuthService> oauth_service;
    RequestRegistry requests;
    std::unique_ptr<WorkPool> auth_pool;
    asio::io_context io{2};
    Tcp::acceptor listener{io};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::vector<std::thread> threads;
    std::mutex sessions_mutex;
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> sessions;
    std::atomic<std::uint64_t> sequence{0};
    std::atomic_bool started{false}, stopped{false};
    Cancel shutdown = std::make_shared<Cancellation>();
    std::uint16_t bound_port = 0;
    Impl(std::shared_ptr<const Config> cfg, std::shared_ptr<McpBackend> handler)
        : config(std::move(cfg)), backend(std::move(handler)), gateway(*config) {
        if (OAuthService::enabled(*config)) {
            oauth_service = std::make_unique<OAuthService>(*config);
            auth_pool = std::make_unique<WorkPool>(4, 64);
        }
    }
    asio::awaitable<void> accept();
    std::uint16_t start();
    void stop();
};
struct HttpServer::Impl::Session : std::enable_shared_from_this<Session> {
    std::shared_ptr<Impl> server;
    beast::tcp_stream stream;
    beast::flat_buffer buffer{64 * 1024};
    HttpRequest request;
    std::string peer;
    Json bridge_headers = Json::object();
    Cancel cancel = std::make_shared<Cancellation>();
    std::uint64_t sequence, sent_bytes = 0;
    int response_status = 0;
    bool response_started = false, completed = false, disconnected = false;
    bool keep_alive = false;
    struct ReadAhead {
        asio::cancellation_signal cancel;
        asio::steady_timer finished;
        bool stopping = false, done = false;
        explicit ReadAhead(asio::any_io_executor executor) : finished(std::move(executor)) {}
    };
    std::shared_ptr<ReadAhead> read_ahead;
    Clock::time_point started_at = Clock::now();
    Session(std::shared_ptr<Impl> owner, Tcp::socket socket, std::uint64_t id)
        : server(std::move(owner)), stream(std::move(socket)), sequence(id) {
        boost::system::error_code ec;
        auto endpoint = stream.socket().remote_endpoint(ec);
        peer = ec ? "" : endpoint.address().to_string();
        request.peer = peer;
        request.cancellation = cancel;
        stream.socket().set_option(Tcp::no_delay(true), ec);
    }
    ~Session() {
        std::lock_guard lock(server->sessions_mutex);
        server->sessions.erase(sequence);
    }
    void close(bool dropped = false) {
        if (dropped && !completed)
            disconnected = true;
        cancel->cancel();
        boost::system::error_code ec;
        stream.socket().shutdown(Tcp::socket::shutdown_both, ec);
        stream.socket().close(ec);
    }
    asio::awaitable<void> monitor_disconnect(std::shared_ptr<ReadAhead> state) {
        try {
            while (!state->stopping && stream.socket().is_open()) {
                const auto remaining = buffer.max_size() - buffer.size();
                if (!remaining) {
                    // Bound pipelined input while an earlier request is active.
                    close(true);
                    break;
                }
                boost::system::error_code ec;
                const auto bytes = co_await stream.socket().async_read_some(
                    buffer.prepare(std::min<std::size_t>(8192, remaining)),
                    asio::bind_cancellation_slot(state->cancel.slot(),
                                                 asio::redirect_error(asio::use_awaitable, ec)));
                buffer.commit(bytes);
                if (state->stopping)
                    break;
                if (ec) {
                    close(true);
                    break;
                }
            }
        } catch (...) {
            close(true);
        }
        state->done = true;
        state->finished.cancel();
    }
    asio::awaitable<void> stop_read_ahead() {
        if (!read_ahead)
            co_return;
        read_ahead->stopping = true;
        read_ahead->cancel.emit(asio::cancellation_type::all);
        while (!read_ahead->done) {
            read_ahead->finished.expires_at(Clock::time_point::max());
            boost::system::error_code ec;
            co_await read_ahead->finished.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        }
        read_ahead.reset();
    }
    asio::awaitable<void> send(HttpReply reply) {
        if (request.method == "POST" && (request.path == "/mcp" || request.path == "/") &&
            reply.status == 200 && reply.body.size() >= 128 * 1024 &&
            json_string(reply.headers, "content-type").starts_with("application/json")) {
            reply.headers["vary"] = "accept-encoding";
            const auto coding = preferred_encoding(json_string(request.headers, "accept-encoding"));
            if (!coding.empty()) {
                auto compressed = co_await compress_json(reply.body, coding, cancel);
                if (compressed) {
                    reply.body = std::move(*compressed);
                    reply.headers["content-encoding"] = coding;
                }
            }
        }
        http::response<http::string_body> response{static_cast<http::status>(reply.status), 11};
        for (auto it = reply.headers.begin(); it != reply.headers.end(); ++it)
            response.set(it.key(), it.value().get<std::string>());
        for (auto it = bridge_headers.begin(); it != bridge_headers.end(); ++it) {
            if (it.key() == "vary" && response.count(http::field::vary))
                response.set(http::field::vary,
                             std::string(response[http::field::vary]) + ", " + it.value().get<std::string>());
            else
                response.set(it.key(), it.value().get<std::string>());
        }
        response.keep_alive(keep_alive && !server->stopped);
        response.body() = request.method == "HEAD" ? "" : std::move(reply.body);
        response.prepare_payload();
        response_status = reply.status;
        response_started = true;
        // Bound each write so large results make progress between I/O
        // completions instead of submitting the entire body in one operation.
        http::response_serializer<http::string_body> serializer(response);
        serializer.limit(64 * 1024);
        boost::system::error_code ec;
        sent_bytes +=
            co_await http::async_write(stream, serializer, asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            disconnected = true;
            throw boost::system::system_error(ec);
        }
        completed = true;
    }
    asio::awaitable<void> send_chunk(std::string text) {
        sent_bytes += co_await asio::async_write(stream.socket(), http::make_chunk(asio::buffer(text)),
                                                 asio::use_awaitable);
    }
    asio::awaitable<void> send_rpc(Json value, bool prefer_json = false) {
        auto reply = HttpReply::json(200, value);
        if (modern_protocol(request) && value.contains("error")) {
            const auto code = value["error"].value("code", 0);
            if (code == -32601)
                reply.status = 404;
            else if (code == -32602 || code == -32021 || code == -32022)
                reply.status = 400;
        }
        if (reply.status == 200 && !prefer_json &&
            json_string(request.headers, "accept").find("text/event-stream") != std::string::npos) {
            reply.body = "event: message\ndata: " + std::move(reply.body) + "\n\n";
            reply.headers["content-type"] = "text/event-stream";
            reply.headers["cache-control"] = "no-cache, no-transform";
            reply.headers["x-accel-buffering"] = "no";
        }
        co_await send(std::move(reply));
    }
    asio::awaitable<Json> call_tool(Json params, Json id) {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string() ||
            (params.contains("arguments") && !params["arguments"].is_null() &&
             !params["arguments"].is_object()))
            throw Error("tools/call requires a name and object arguments");
        const auto name = json_string(params, "name");
        const auto args = !params.contains("arguments") || params["arguments"].is_null()
                              ? Json::object()
                              : params["arguments"];
        Json context{{"request_id", id}};
        for (const auto& [header, key] :
             {std::pair{"mcp-session-id", "session_id"}, std::pair{"user-agent", "user_agent"}}) {
            auto value = json_string(request.headers, header);
            if (!trim(value).empty())
                context[key] = value;
        }
        if (request.oauth)
            context["client_id"] = json_string(*request.oauth, "clientId");
        const auto invocation = server->backend->tool_started(name, args, context);
        try {
            Json result;
            const auto scope = required_tool_scope(name);
            if (request.oauth && scope && !oauth_scope_allows(json_strings(*request.oauth, "scopes"), *scope))
                result = result_error(
                    "OAuth token is missing the required scope " + *scope + " for tool " + name + ".",
                    Json{{"requiredScope", *scope}, {"clientId", json_string(*request.oauth, "clientId")}});
            else
                result = co_await server->backend->call_tool(name, args, cancel);
            server->backend->tool_finished(invocation, result);
            if (modern_protocol(request))
                result["resultType"] = "complete";
            co_return result;
        } catch (const std::exception& e) {
            server->backend->tool_failed(invocation, e.what());
            throw;
        }
    }
    asio::awaitable<void> tool_response(const Json& id, const Json& params, bool sse) {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string() ||
            (params.contains("arguments") && !params["arguments"].is_null() &&
             !params["arguments"].is_object())) {
            co_await send_rpc(rpc_error(id, -32602, "tools/call requires a name and object arguments"));
            co_return;
        }
        if (!server->backend->has_tool(json_string(params, "name"))) {
            co_await send_rpc(rpc_error(id, -32602, "tool not found"));
            co_return;
        }
        auto registration = server->requests.register_request(request, id, cancel);
        Json response;
        if (sse) {
            struct Completion {
                asio::steady_timer timer;
                bool finished = false;
                Json response;
                explicit Completion(asio::any_io_executor executor) : timer(std::move(executor)) {}
            };
            auto completion = std::make_shared<Completion>(co_await asio::this_coro::executor);
            const auto self = shared_from_this();
            auto next_heartbeat = Clock::now() + Millis(1000);
            asio::co_spawn(stream.get_executor(), call_tool(params, id),
                           [completion, self, id](std::exception_ptr error, Json result) {
                               if (error) {
                                   try {
                                       std::rethrow_exception(error);
                                   } catch (const Cancelled& e) {
                                       completion->response = rpc_error(id, -32800, e.what());
                                   } catch (const std::exception& e) {
                                       completion->response = rpc_error(id, -32603, e.what());
                                   }
                               } else
                                   completion->response = rpc_result(id, std::move(result));
                               completion->finished = true;
                               completion->timer.cancel();
                           });
            // Completion wakes this wait immediately. A ready result can use
            // ordinary JSON when the client accepts it, avoiding SSE framing
            // and parsing for large values. Slower operations still stream
            // within a bounded grace period and retain their heartbeats.
            if (!completion->finished) {
                completion->timer.expires_after(Millis(100));
                boost::system::error_code initial_ec;
                co_await completion->timer.async_wait(asio::redirect_error(asio::use_awaitable, initial_ec));
            }
            if (completion->finished) {
                if (!disconnected) {
                    const auto use_json = !completion->response.contains("error") &&
                                          prefers_json(json_string(request.headers, "accept"));
                    co_await send_rpc(std::move(completion->response), use_json);
                }
                co_return;
            }
            http::response<http::empty_body> headers{http::status::ok, 11};
            headers.set(http::field::content_type, "text/event-stream");
            headers.set(http::field::cache_control, "no-cache, no-transform");
            headers.set("x-accel-buffering", "no");
            headers.chunked(true);
            headers.keep_alive(keep_alive && !server->stopped);
            for (auto it = bridge_headers.begin(); it != bridge_headers.end(); ++it)
                headers.set(it.key(), it.value().get<std::string>());
            http::response_serializer<http::empty_body> serializer{headers};
            response_started = true;
            response_status = 200;
            sent_bytes += co_await http::async_write_header(stream, serializer, asio::use_awaitable);
            co_await send_chunk(": mcp-request-start\n\n");
            while (!completion->finished) {
                completion->timer.expires_at(next_heartbeat);
                boost::system::error_code ec;
                co_await completion->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                if (!completion->finished && !cancel->cancelled())
                    co_await send_chunk(": heartbeat\n\n");
                next_heartbeat = Clock::now() + Millis(1000);
            }
            response = std::move(completion->response);
            if (disconnected)
                co_return;
            co_await send_chunk(
                "event: message\ndata: " + json_dump(response, Json::error_handler_t::replace) + "\n\n");
            sent_bytes +=
                co_await asio::async_write(stream.socket(), http::make_chunk_last(), asio::use_awaitable);
            completed = true;
        } else {
            try {
                response = rpc_result(id, co_await call_tool(params, id));
            } catch (const Cancelled& e) {
                response = rpc_error(id, -32800, e.what());
            } catch (const std::exception& e) {
                response = rpc_error(id, -32603, e.what());
            }
            if (!disconnected)
                co_await send(HttpReply::json(200, response));
        }
    }
    asio::awaitable<void> dispatch() {
        auto decision = server->gateway.inspect(request);
        request.is_local = decision.is_local;
        bridge_headers = std::move(decision.response_headers);
        if (decision.response) {
            co_await send(std::move(*decision.response));
            co_return;
        }
        const bool get = request.method == "GET" || request.method == "HEAD";
        if (get && (request.path == "/healthz" || request.path == "/livez")) {
            co_await send(HttpReply::text(200, "ok"));
            co_return;
        }
        if (get && request.path == "/readyz") {
            const bool ready = server->backend->ready();
            const Json body{{"ok", ready}};
            co_await send(HttpReply::json(ready ? 200 : 503, body));
            co_return;
        }
        auto* oauth = server->oauth_service.get();
        if (oauth && get && request.path == "/.well-known/oauth-authorization-server") {
            co_await send(HttpReply::json(200, oauth->authorization_server_metadata()));
            co_return;
        }
        if (oauth && get &&
            (request.path == "/.well-known/oauth-protected-resource" ||
             request.path == "/.well-known/oauth-protected-resource/mcp")) {
            co_await send(
                HttpReply::json(200, oauth->protected_resource_metadata(request.path.ends_with("/mcp"))));
            co_return;
        }
        if (oauth &&
            ((request.method == "POST" && (request.path == "/register" || request.path == "/token" ||
                                           request.path == "/revoke" || request.path == "/authorize")) ||
             (get && request.path == "/authorize"))) {
            if (request.method == "POST") {
                const auto type = lower(json_string(request.headers, "content-type"));
                const auto expected =
                    request.path == "/register" ? "application/json" : "application/x-www-form-urlencoded";
                if (!type.starts_with(expected)) {
                    co_await send(HttpReply::text(415, "Unsupported Content-Type"));
                    co_return;
                }
            }
            auto self = shared_from_this();
            auto pending = server->auth_pool->run(
                [self, oauth] { return oauth_route(*oauth, self->request, self->cancel); }, cancel);
            auto reply = co_await std::move(pending);
            co_await send(std::move(reply));
            co_return;
        }
        const bool mcp = request.path == "/" || request.path == "/mcp";
        if (!mcp) {
            co_await send(HttpReply::text(404, "Not Found"));
            co_return;
        }
        if (oauth && (request.method == "POST" || request.method == "DELETE")) {
            std::optional<HttpReply> failure;
            try {
                const auto authorization = json_string(request.headers, "authorization");
                if (authorization.empty())
                    throw OAuthFailure("invalid_token", "Missing Authorization header", 401);
                const auto space = authorization.find(' ');
                if (space == authorization.npos || lower(authorization.substr(0, space)) != "bearer" ||
                    space + 1 == authorization.size())
                    throw OAuthFailure("invalid_token",
                                       "Invalid Authorization header format, expected 'Bearer TOKEN'", 401);
                const auto token = authorization.substr(space + 1);
                auto pending = server->auth_pool->run(
                    [oauth, token] { return oauth->verify_access_token(token); }, cancel);
                auto info = co_await std::move(pending);
                request.oauth.emplace(std::move(info));
            } catch (const OAuthFailure& e) {
                failure.emplace(oauth_failure(e));
                const auto escaped = replace_all(replace_all(e.what(), "\\", "\\\\"), "\"", "\\\"");
                failure->headers["www-authenticate"] =
                    "Bearer error=\"" + e.code + "\", error_description=\"" + escaped +
                    "\", resource_metadata=\"" + oauth->resource_metadata_url(request.path == "/mcp") + "\"";
            }
            if (failure) {
                co_await send(std::move(*failure));
                co_return;
            }
        }
        if (request.method == "DELETE") {
            co_await send(HttpReply::text(200, ""));
            co_return;
        }
        const auto accept = json_string(request.headers, "accept");
        if (get) {
            if (accept.find("text/event-stream") != accept.npos) {
                HttpReply probe{200, ": mcp-sse-probe\n\n",
                                Json{{"content-type", "text/event-stream"},
                                     {"cache-control", "no-cache, no-transform"},
                                     {"x-accel-buffering", "no"}}};
                co_await send(std::move(probe));
            } else if (request.path == "/") {
                auto metadata = co_await server->backend->metadata(request);
                metadata["gateway_bridge"] = server->gateway.bridge_info(request.is_local);
                if (oauth)
                    metadata["oauth"] = oauth->oauth_info();
                co_await send(HttpReply::json(200, metadata));
            } else
                co_await send(HttpReply::json(
                    406, rpc_error(nullptr, -32000, "Not Acceptable: Client must accept text/event-stream")));
            co_return;
        }
        if (request.method != "POST") {
            co_await send(HttpReply::text(405, "Method Not Allowed"));
            co_return;
        }
        if (!lower(json_string(request.headers, "content-type")).starts_with("application/json")) {
            co_await send(HttpReply::json(
                415,
                rpc_error(nullptr, -32000, "Unsupported Media Type: Content-Type must be application/json")));
            co_return;
        }
        const auto protocol = json_string(request.headers, "mcp-protocol-version", "2025-03-26");
        if (accept.find("application/json") == accept.npos &&
            accept.find("text/event-stream") == accept.npos && accept != "*/*") {
            co_await send(HttpReply::json(
                406, rpc_error(nullptr, -32000,
                               "Not Acceptable: Client must accept application/json or text/event-stream")));
            co_return;
        }
        Json body;
        std::optional<HttpReply> parse_failure;
        try {
            body = bounded_json(request.body);
        } catch (...) {
            parse_failure.emplace(HttpReply::json(400, rpc_error(nullptr, -32700, "Parse error")));
        }
        if (parse_failure) {
            co_await send(std::move(*parse_failure));
            co_return;
        }
        if (!body.is_object() || json_string(body, "jsonrpc") != "2.0" || !body.contains("method") ||
            !body["method"].is_string() ||
            (body.contains("id") && !body["id"].is_string() && !body["id"].is_number_integer())) {
            co_await send(HttpReply::json(400, rpc_error(nullptr, -32600, "Invalid Request")));
            co_return;
        }
        const auto method = json_string(body, "method");
        const auto params = body.value("params", Json::object());
        if (auto invalid = validate_protocol_request(request, body)) {
            co_await send(std::move(*invalid));
            co_return;
        }
        if (!body.contains("id")) {
            if (method == "notifications/cancelled" && params.is_object() && params.contains("requestId"))
                server->requests.cancel(request, params["requestId"]);
            co_await send(HttpReply::text(202, ""));
            co_return;
        }
        const auto id = body["id"];
        if (method == "initialize") {
            const auto requested = json_string(params, "protocolVersion");
            auto result = Json{{"protocolVersion", supported_protocol(requested) ? requested : "2025-11-25"},
                               {"capabilities", capabilities()},
                               {"serverInfo", server->backend->server_info()}};
            co_await send_rpc(rpc_result(id, result));
        } else if (method == "server/discover") {
            const Json result{
                {"resultType", "complete"},
                {"supportedVersions", supported_protocols()},
                {"capabilities", capabilities()},
                {"ttlMs", 0},
                {"cacheScope", "private"},
                {"_meta", {{"io.modelcontextprotocol/serverInfo", server->backend->server_info()}}}};
            co_await send_rpc(rpc_result(id, result));
        } else if (method == "ping" && !modern_protocol(request))
            co_await send_rpc(rpc_result(id, Json::object()));
        else if (method == "tools/list") {
            // Materialize initializer lists before suspension (also supports GCC 12 coroutines).
            Json result = Json::object();
            if (modern_protocol(request)) {
                result["resultType"] = "complete";
                result["ttlMs"] = 0;
                result["cacheScope"] = "public";
            }
            result["tools"] = server->backend->list_tools(protocol);
            co_await send_rpc(rpc_result(id, result));
        } else if (method == "tools/call")
            co_await tool_response(id, params, accept.find("text/event-stream") != accept.npos);
        else if (method == "resources/list" || method == "resources/templates/list" ||
                 method == "prompts/list") {
            Json result = Json::object();
            if (modern_protocol(request))
                result["resultType"] = "complete";
            const auto field = method == "resources/list"             ? "resources"
                               : method == "resources/templates/list" ? "resourceTemplates"
                                                                      : "prompts";
            result[field] = Json::array();
            co_await send_rpc(rpc_result(id, result));
        } else
            co_await send_rpc(rpc_error(id, -32601, method));
    }
    asio::awaitable<void> run() {
        while (!server->stopped && stream.socket().is_open()) {
            // Authentication, CORS, cancellation and response state belong to a
            // request, not to the persistent TCP connection.
            request = HttpRequest{};
            request.peer = peer;
            cancel = std::make_shared<Cancellation>();
            request.cancellation = cancel;
            bridge_headers = Json::object();
            response_started = completed = disconnected = keep_alive = false;
            response_status = 0;
            sent_bytes = 0;
            std::optional<HttpReply> failure;
            try {
                stream.expires_after(Millis(15000));
                boost::system::error_code ec;
                if (!buffer.size()) {
                    const auto bytes = co_await stream.async_read_some(
                        buffer.prepare(8192), asio::redirect_error(asio::use_awaitable, ec));
                    buffer.commit(bytes);
                    if (ec)
                        break;
                }
                // Exclude an idle keep-alive interval from the next request's
                // handling duration. Buffered pipeline bytes are never discarded.
                started_at = Clock::now();
                http::request_parser<http::string_body> parser;
                parser.header_limit(32768);
                parser.body_limit(server->config->mcp_json_body_limit_bytes);
                parser.eager(true);
                // Parse bytes already read before creating another asynchronous
                // read operation. Consume exactly this message's bytes so any
                // following pipelined request remains in the bounded buffer.
                const auto used = parser.put(buffer.data(), ec);
                buffer.consume(used);
                if (ec == http::error::need_more)
                    ec.clear();
                if (!ec && !parser.is_done())
                    co_await http::async_read(stream, buffer, parser,
                                              asio::redirect_error(asio::use_awaitable, ec));
                if (ec == http::error::body_limit) {
                    // GCC 12/13 cannot lower this initializer-list temporary
                    // inside co_await in the request loop. Give it a local owner.
                    auto oversized = HttpReply::json(413, Json{{"error", "request entity too large"}});
                    co_await send(std::move(oversized));
                } else if (ec) {
                    if (ec != http::error::end_of_stream && ec != asio::error::operation_aborted)
                        failure.emplace(HttpReply::text(400, "Invalid HTTP request"));
                } else {
                    auto parsed = parser.release();
                    keep_alive = parsed.keep_alive();
                    request.method = std::string(parsed.method_string());
                    request.target = std::string(parsed.target());
                    const auto q = request.target.find('?');
                    request.path = request.target.substr(0, q);
                    if (q != request.target.npos)
                        request.query = request.target.substr(q + 1);
                    request.body = std::move(parsed.body());
                    for (const auto& header : parsed.base())
                        request.headers[lower(std::string(header.name_string()))] =
                            std::string(header.value());
                    stream.expires_never();
                    const bool probe =
                        (request.method == "GET" || request.method == "HEAD") &&
                        (request.path == "/healthz" || request.path == "/livez" || request.path == "/readyz");
                    if (!probe) {
                        const auto self = shared_from_this();
                        read_ahead = std::make_shared<ReadAhead>(stream.get_executor());
                        asio::co_spawn(stream.get_executor(), monitor_disconnect(read_ahead),
                                       [self](std::exception_ptr) {});
                    }
                    co_await dispatch();
                }
            } catch (const Cancelled&) {
                disconnected = true;
            } catch (const std::exception& e) {
                if (!response_started && !disconnected)
                    failure.emplace(HttpReply::json(500, Json{{"error", e.what()}}));
            }
            if (read_ahead)
                co_await stop_read_ahead();
            if (failure && !response_started && stream.socket().is_open()) {
                try {
                    co_await send(std::move(*failure));
                } catch (...) {
                }
            }
            const auto duration = std::chrono::duration_cast<Millis>(Clock::now() - started_at);
            try {
                server->backend->observe_http(request, response_status, sent_bytes, duration, disconnected);
            } catch (...) {
            }
            cancel->cancel();
            if (!completed || !keep_alive || disconnected || server->stopped)
                break;
        }
        close();
    }
};
asio::awaitable<void> HttpServer::Impl::accept() {
    while (!stopped) {
        boost::system::error_code ec;
        auto socket = co_await listener.async_accept(asio::make_strand(io),
                                                     asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (stopped || ec == asio::error::operation_aborted)
                co_return;
            co_await async_delay(Millis(25));
            continue;
        }
        const auto id = ++sequence;
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(sessions_mutex);
            if (sessions.size() >= 512) {
                socket.close(ec);
                continue;
            }
            session = std::make_shared<Session>(shared_from_this(), std::move(socket), id);
            sessions[id] = session;
        }
        asio::co_spawn(session->stream.get_executor(), session->run(), [session](std::exception_ptr) {});
    }
}
std::uint16_t HttpServer::Impl::start() {
    if (started.exchange(true))
        return bound_port;
    const auto endpoints =
        Tcp::resolver(io).resolve(config->host, std::to_string(config->port), Tcp::resolver::passive);
    if (endpoints.empty())
        throw Error("No listener endpoint resolved");
    const auto endpoint = endpoints.begin()->endpoint();
    listener.open(endpoint.protocol());
#ifdef _WIN32
    BOOL exclusive = TRUE;
    if (::setsockopt(listener.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
        throw Error("Unable to set exclusive listener ownership");
#else
    listener.set_option(Tcp::acceptor::reuse_address(true));
#endif
    listener.bind(endpoint);
    listener.listen(128);
    bound_port = listener.local_endpoint().port();
    work.emplace(asio::make_work_guard(io));
    asio::co_spawn(io, accept(), [self = shared_from_this()](std::exception_ptr error) {
        if (error && !self->stopped) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                std::cerr << "Listener failed: " << e.what() << '\n';
            }
        }
    });
    for (int i = 0; i < 2; ++i)
        threads.emplace_back([self = shared_from_this()] { self->io.run(); });
    return bound_port;
}
void HttpServer::Impl::stop() {
    shutdown->cancel();
    if (stopped.exchange(true) || threads.empty())
        return;
    requests.cancel_all();
    asio::post(io, [self = shared_from_this()] {
        boost::system::error_code ec;
        self->listener.cancel(ec);
        self->listener.close(ec);
        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::lock_guard lock(self->sessions_mutex);
            for (const auto& [id, weak] : self->sessions) {
                (void)id;
                if (auto live = weak.lock())
                    sessions.push_back(live);
            }
        }
        for (auto& session : sessions)
            asio::dispatch(session->stream.get_executor(), [session] { session->close(true); });
        self->work.reset();
    });
    for (auto& thread : threads)
        if (thread.joinable())
            thread.join();
}
HttpServer::HttpServer(std::shared_ptr<const Config> config, std::shared_ptr<McpBackend> backend)
    : impl_(std::make_shared<Impl>(std::move(config), std::move(backend))) {}
HttpServer::~HttpServer() {
    stop();
}
std::uint16_t HttpServer::start() {
    return impl_->start();
}
void HttpServer::stop() {
    impl_->stop();
}
std::uint16_t HttpServer::port() const {
    return impl_->bound_port;
}
std::size_t HttpServer::active_requests() const {
    return impl_->requests.active_count();
}
asio::any_io_executor HttpServer::executor() const {
    return impl_->io.get_executor();
}
Cancel HttpServer::stop_token() const {
    return impl_->shutdown;
}
} // namespace devbox
