#include "devbox/provider.hpp"
#include "devbox/state_store.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using Tcp = asio::ip::tcp;
void require(bool value, const char* text) {
    if (!value)
        throw Error(text);
}
template <class F> void rejects(F&& fn, std::string_view expected) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos)
            return;
        throw Error("Unexpected rejection: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(expected));
}
Json response(std::string text = "fresh answer", bool tool = false) {
    Json result{{"id", "resp_fixture"},
                {"model", "fixture-model-revision"},
                {"status", "completed"},
                {"usage", {{"input_tokens", 10}, {"output_tokens", 5}}},
                {"output", Json::array()}};
    if (tool)
        result["output"].push_back(Json{{"type", "function_call"},
                                        {"id", "fc_one"},
                                        {"call_id", "call_one"},
                                        {"name", "lookup"},
                                        {"arguments", "{\"path\":\"ok\"}"}});
    else
        result["output"].push_back(
            Json{{"type", "message"},
                 {"role", "assistant"},
                 {"content", Json::array({Json{{"type", "output_text"}, {"text", text}}})}});
    return result;
}
std::string event(const Json& value) {
    return "data: " + value.dump() + "\n\n";
}
std::string response_stream(bool complete = true) {
    auto stream = event(Json{{"type", "response.created"}, {"response", {{"id", "resp_fixture"}}}});
    stream += event(Json{{"type", "response.output_item.added"},
                         {"item",
                          {{"type", "function_call"},
                           {"id", "fc_one"},
                           {"call_id", "call_one"},
                           {"name", "lookup"},
                           {"arguments", ""}}}});
    stream += event(Json{
        {"type", "response.function_call_arguments.delta"}, {"item_id", "fc_one"}, {"delta", "{\"path\":"}});
    if (!complete)
        return stream;
    stream += event(Json{
        {"type", "response.function_call_arguments.delta"}, {"item_id", "fc_one"}, {"delta", "\"ok\"}"}});
    stream += event(Json{{"type", "response.function_call_arguments.done"},
                         {"item_id", "fc_one"},
                         {"arguments", "{\"path\":\"ok\"}"}});
    stream += event(Json{{"type", "response.completed"}, {"response", response("", true)}});
    return stream;
}
void protocol_tests() {
    const auto stream = response_stream();
    ProviderStream parser(ProviderProtocol::Responses);
    for (const auto byte : stream)
        parser.feed(std::string_view(&byte, 1));
    const auto parsed = parser.finish();
    require(parsed.status == "completed" && parsed.calls.size() == 1 &&
                parsed.calls[0].arguments["path"] == "ok" && parsed.usage.input_tokens == 10 &&
                !parsed.billing_unknown,
            "fragmented response binds complete arguments and authoritative usage");
    require(parser.finish().calls.size() == 1, "finish is stable on repeated reads");
    ProviderStream interrupted(ProviderProtocol::Responses);
    interrupted.feed(response_stream(false));
    const auto incomplete = interrupted.finish();
    require(incomplete.status == "interrupted" && incomplete.calls.empty() && incomplete.billing_unknown &&
                incomplete.response_id == "resp_fixture",
            "partial tool arguments cannot reach execution and retain a reconnect identity");
    ProviderStream invalid(ProviderProtocol::Responses);
    rejects(
        [&] {
            invalid.feed(event(Json{{"type", "response.function_call_arguments.delta"},
                                    {"item_id", "unknown"},
                                    {"delta", "{}"}}));
        },
        "UNBOUND");
    ProviderStream bounded(ProviderProtocol::Responses, 4);
    rejects([&] { bounded.feed("12345"); }, "BUDGET");
    auto duplicate = response("", true);
    duplicate["output"].push_back(duplicate["output"][0]);
    rejects([&] { parse_provider_response(ProviderProtocol::Responses, duplicate); }, "TOOL_IDENTITY");
    auto escalation = response();
    escalation["output"][0]["role"] = "system";
    rejects([&] { parse_provider_response(ProviderProtocol::Responses, escalation); }, "ROLE_ESCALATION");
    ProviderStream chat(ProviderProtocol::ChatCompletions);
    const auto chunk = [](const Json& delta, const Json& reason = Json()) {
        return Json{
            {"id", "chat_fixture"},
            {"model", "local-fixture"},
            {"choices", Json::array({Json{{"index", 0}, {"delta", delta}, {"finish_reason", reason}}})}};
    };
    chat.feed(event(
        chunk(Json{{"tool_calls",
                    Json::array({Json{{"index", 0},
                                      {"id", "call_local"},
                                      {"function", {{"name", "lookup"}, {"arguments", "{\"path\":"}}}}})}})));
    chat.feed(event(chunk(
        Json{{"tool_calls", Json::array({Json{{"index", 0}, {"function", {{"arguments", "\"ok\"}"}}}}})}},
        "tool_calls")));
    chat.feed(event(Json{{"id", "chat_fixture"},
                         {"choices", Json::array()},
                         {"usage", {{"prompt_tokens", 12}, {"completion_tokens", 8}}}}));
    chat.feed("data: [DONE]\n\n");
    require(chat.finish().calls[0].arguments["path"] == "ok" && chat.finish().usage.output_tokens == 8,
            "local-compatible streams accept usage-only terminal chunks and fragmented calls");
    ProviderStream bad_chat(ProviderProtocol::ChatCompletions);
    bad_chat.feed(
        event(chunk(
            Json{{"tool_calls", Json::array({Json{{"index", 0},
                                                  {"id", "one"},
                                                  {"function", {{"name", "lookup"}, {"arguments", "{}"}}}},
                                             Json{{"index", 1},
                                                  {"id", "two"},
                                                  {"function", {{"name", "lookup"}, {"arguments", "{"}}}}})}},
            "tool_calls")) +
        "data: [DONE]\n\n");
    bool malformed = false;
    try {
        (void)bad_chat.finish();
    } catch (const std::exception&) {
        malformed = true;
    }
    require(malformed && bad_chat.finish().calls.empty() && bad_chat.finish().status == "protocol_error",
            "one malformed call cannot expose earlier valid calls from the same unfinished batch");
    ProviderProfile profile;
    profile.id = "local";
    profile.model = "fixture";
    profile.local = true;
    profile.tools = true;
    ProviderRequest request;
    request.messages = Json::array({Json{{"role", "user"}, {"content", "Research a current price"}}});
    require(provider_payload(profile, request)["input"][0]["role"] == "user", "portable user role retained");
    request.tools = Json::array({Json{
        {"type", "function"}, {"function", {{"name", "lookup"}, {"parameters", {{"type", "object"}}}}}}});
    profile.tools = false;
    rejects([&] { provider_payload(profile, request); }, "TOOLS_UNSUPPORTED");
    profile.tools = true;
    profile.required_vram_bytes = 2;
    rejects([&] { provider_payload(profile, request); }, "VRAM_BUDGET");
    profile.required_vram_bytes = 0;
    request.messages[0]["content"] =
        Json::array({Json{{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AA=="}}}}});
    rejects([&] { provider_payload(profile, request); }, "VISION_UNSUPPORTED");
    request.messages[0]["content"] = std::string(10000, 'x');
    rejects([&] { provider_payload(profile, request); }, "CONTEXT_BUDGET");
    request.messages[0]["content"] = "small";
    profile.local = false;
    rejects([&] { provider_payload(profile, request); }, "TARIFF_CEILING_REQUIRED");
    profile.input_micro_usd_per_million = 2000000;
    profile.output_micro_usd_per_million = 4000000;
    rejects([&] { provider_payload(profile, request); }, "COST_BUDGET");
    require(provider_cost_ceiling(profile, 10, 5) == 40, "integer conservative cost arithmetic");
    rejects([&] { provider_cost_ceiling(profile, UINT64_MAX, UINT64_MAX); }, "OVERFLOW");
}
struct Fixture {
    asio::io_context io;
    Tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic_bool stop{false};
    std::atomic_int mode{0}, calls{0}, generations{0};
    std::atomic_bool saw_auth{false}, bad_auth{false};
    std::string secret = "SYNTHETIC-PROVIDER-CREDENTIAL";
    std::thread worker;
    Fixture() {
        acceptor.non_blocking(true);
        worker = std::thread([this] {
            while (!stop) {
                boost::system::error_code error;
                Tcp::socket socket(io);
                acceptor.accept(socket, error);
                if (error) {
                    std::this_thread::sleep_for(Millis(2));
                    continue;
                }
                try {
                    boost::beast::flat_buffer buffer;
                    http::request<http::string_body> request;
                    http::read(socket, buffer, request);
                    ++calls;
                    if (request.method() == http::verb::post && request.target() == "/v1/responses")
                        ++generations;
                    if (!request[http::field::authorization].empty()) {
                        saw_auth = true;
                        bad_auth = request[http::field::authorization] != "Bearer " + secret;
                    }
                    const auto choice = mode.load();
                    http::response<http::string_body> reply{http::status::ok, 11};
                    reply.keep_alive(false);
                    reply.set(http::field::content_type, "application/json");
                    if (std::string(request.target()).ends_with("/cancel")) {
                        auto value = response();
                        value["status"] = "cancelled";
                        value["usage"] = nullptr;
                        reply.body() = value.dump();
                    } else if (choice == 1) {
                        reply.result(http::status::too_many_requests);
                        reply.set(http::field::retry_after, "120");
                        reply.body() = "{}";
                    } else if (choice == 2) {
                        reply.result(http::status::found);
                        reply.set(http::field::location, "http://169.254.169.254/");
                        reply.body() = "{}";
                    } else if (choice == 3 || choice == 4 || choice == 5) {
                        reply.set(http::field::content_type, "text/event-stream");
                        reply.body() = response_stream(choice == 3);
                    } else
                        reply.body() = response(choice == 6 ? secret : "fresh answer").dump();
                    reply.prepare_payload();
                    if (choice == 5) {
                        const std::string header =
                            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
                        asio::write(socket, asio::buffer(header));
                        asio::write(socket,
                                    asio::buffer(event(Json{{"type", "response.created"},
                                                            {"response", {{"id", "resp_fixture"}}}})));
                        const auto deadline = Clock::now() + Millis(1800);
                        while (!stop && Clock::now() < deadline)
                            std::this_thread::sleep_for(Millis(10));
                    } else
                        http::write(socket, reply);
                } catch (...) {
                }
            }
        });
    }
    ~Fixture() {
        stop = true;
        worker.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/v1";
    }
};
void network_tests(const fs::path& root) {
    Fixture fixture;
    ProviderProfile profile;
    profile.id = "fixture";
    profile.model = "fixture-model";
    profile.local = true;
    profile.base_url = fixture.url();
    profile.tools = true;
    profile.background_cancel = true;
    ModelProvider provider(profile);
    const SecretScope scope{"principal", "run", profile.base_url};
    ProviderRequest request;
    request.messages = Json::array({Json{{"role", "user"}, {"content", "fixture task"}}});
    request.tools = Json::array({Json{
        {"type", "function"}, {"function", {{"name", "lookup"}, {"parameters", {{"type", "object"}}}}}}});
    auto result = provider.generate(request, scope);
    require(result.status == "completed" && result.text == "fresh answer" &&
                result.usage.cost_ceiling_micro_usd == 0,
            "real native transport returns local inference text and usage");
    fixture.mode = 3;
    result = provider.generate(request, scope);
    require(result.calls.size() == 1 && result.calls[0].name == "lookup",
            "native HTTP streaming assembles a tool call");
    auto undeclared = request;
    undeclared.tools = Json::array();
    const auto unsolicited = provider.generate(undeclared, scope);
    require(unsolicited.status == "protocol_error" && unsolicited.calls.empty(),
            "provider cannot invent an undeclared executable tool");
    fixture.mode = 4;
    result = provider.generate(request, scope);
    require(result.status == "interrupted" && result.calls.empty() && result.billing_unknown,
            "HTTP EOF cannot complete a partial tool call");
    const auto generations = fixture.generations.load();
    fixture.mode = 0;
    require(provider.retrieve(result.response_id, scope).status == "completed" &&
                fixture.generations == generations,
            "reconnect retrieves an existing response without another generation POST");
    require(provider.cancel_background("resp_fixture", scope).remote_cancel_confirmed,
            "remote cancelled state is distinct from transport cancellation");
    fixture.mode = 1;
    const auto before = fixture.calls.load();
    result = provider.generate(request, scope);
    require(result.status == "rate_limited" && result.retry_after_ms == 120000 && fixture.calls == before + 1,
            "rate limits have explicit delay and no automatic billable retry");
    fixture.mode = 2;
    result = provider.generate(request, scope);
    require(result.status == "rejected" && result.http_status == 302,
            "provider redirects cannot move a scoped credential or bypass egress");
    SecretBroker secrets;
    atomic_write(root / "provider-key", fixture.secret);
    profile.secret_reference = secrets.bind(root / "provider-key", scope, unix_millis() + 60000);
    ModelProvider keyed(profile, &secrets);
    fixture.mode = 6;
    std::string emitted;
    result = keyed.generate(request, scope, {}, [&](std::string_view text) { emitted += text; });
    require(fixture.saw_auth && !fixture.bad_auth && result.status == "protocol_error" &&
                result.json().dump().find(fixture.secret) == std::string::npos && emitted.empty(),
            "keys stay in authorized headers and echoed credentials never enter events or model context");
    auto other = scope;
    other.run = "other-run";
    rejects([&] { keyed.generate(request, other); }, "scope");
    secrets.revoke(*profile.secret_reference);
    rejects([&] { keyed.generate(request, scope); }, "scope");
    fixture.mode = 5;
    const auto cancel = std::make_shared<Cancellation>();
    const auto began = Clock::now();
    auto running = std::async(std::launch::async, [&] { return provider.generate(request, scope, cancel); });
    std::this_thread::sleep_for(Millis(100));
    cancel->cancel();
    result = running.get();
    require(result.status == "cancel_requested" && !result.remote_cancel_confirmed &&
                result.billing_unknown && result.response_id == "resp_fixture" &&
                Clock::now() - began < Millis(1500),
            "cancel stops local transport without claiming a remote stop or known billing");
    auto forbidden = profile;
    forbidden.secret_reference.reset();
    forbidden.local = true;
    forbidden.base_url = "http://localhost:1234/v1";
    rejects([&] { ModelProvider invalid(forbidden); }, "LITERAL_LOOPBACK");
    forbidden.local = false;
    forbidden.base_url = "http://api.example.org/v1";
    rejects([&] { ModelProvider invalid(forbidden); }, "HTTPS_REQUIRED");
}
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-provider-" + uuid());
    ensure_private_state_directory(root);
    try {
        protocol_tests();
        network_tests(root);
        fs::remove_all(root);
        std::cout << "Provider protocols, streaming fragments, budgets, scopes, cancellation, replay and "
                     "secret isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
