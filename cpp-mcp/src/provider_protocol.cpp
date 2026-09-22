#include "devbox/provider.hpp"
#include "devbox/resource_budget.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace devbox {
namespace {
Json parse_bounded(std::string_view bytes, std::size_t maximum = 65536) {
    if (bytes.size() > maximum)
        throw Error("PROVIDER_JSON_BUDGET");
    std::size_t nodes = 0;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t, Json&) {
        if (depth > 64 || ++nodes > 131072)
            throw Error("PROVIDER_JSON_SHAPE_BUDGET");
        return true;
    });
}
bool identifier(std::string_view text) {
    return !text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
}
void add_call(ProviderResult& result, std::string id, std::string name, std::string raw) {
    if (!identifier(id) || !identifier(name) || result.calls.size() >= 32 ||
        std::any_of(result.calls.begin(), result.calls.end(),
                    [&](const auto& call) { return call.id == id; }))
        throw Error("PROVIDER_INVALID_TOOL_IDENTITY");
    auto arguments = parse_bounded(raw);
    if (!arguments.is_object())
        throw Error("PROVIDER_TOOL_ARGUMENTS_MUST_BE_OBJECT");
    result.calls.push_back({std::move(id), std::move(name), std::move(arguments)});
}
void usage(ProviderResult& result, const Json& value, bool responses) {
    if (!value.is_object())
        return;
    const auto* input = responses ? "input_tokens" : "prompt_tokens";
    const auto* output = responses ? "output_tokens" : "completion_tokens";
    if (value.contains(input) && !value[input].is_null())
        result.usage.input_tokens = json_uint(value, input);
    if (value.contains(output) && !value[output].is_null())
        result.usage.output_tokens = json_uint(value, output);
    result.billing_unknown = !result.usage.input_tokens || !result.usage.output_tokens;
}
Json response_input(const Json& messages, bool vision) {
    Json input = Json::array();
    for (const auto& message : messages) {
        const auto role = json_string(message, "role");
        if (message.contains("native_items")) {
            if (role != "assistant" || !message["native_items"].is_array())
                throw Error("PROVIDER_NATIVE_ITEMS_ROLE");
            const auto validated =
                parse_provider_response(ProviderProtocol::Responses,
                                        Json{{"status", "completed"}, {"output", message["native_items"]}});
            for (const auto& item : validated.native_output)
                input.push_back(item);
            continue;
        }
        if (role == "tool") {
            const auto id = json_string(message, "tool_call_id");
            if (!identifier(id))
                throw Error("PROVIDER_INVALID_TOOL_IDENTITY");
            input.push_back(Json{{"type", "function_call_output"},
                                 {"call_id", id},
                                 {"output", json_string(message, "content")}});
            continue;
        }
        if (role != "system" && role != "developer" && role != "user" && role != "assistant")
            throw Error("PROVIDER_MESSAGE_ROLE");
        if (message.contains("content") && !message["content"].is_null()) {
            if (message["content"].is_string())
                input.push_back(Json{{"role", role}, {"content", message["content"]}});
            else {
                if (!vision || role != "user" || !message["content"].is_array())
                    throw Error("PROVIDER_VISION_UNSUPPORTED");
                Json content = Json::array();
                for (const auto& part : message["content"]) {
                    const auto type = json_string(part, "type");
                    if (type == "text")
                        content.push_back(Json{{"type", "input_text"}, {"text", json_string(part, "text")}});
                    else if (type == "image_url")
                        content.push_back(
                            Json{{"type", "input_image"}, {"image_url", part.at("image_url").at("url")}});
                    else
                        throw Error("PROVIDER_CONTENT_TYPE_UNSUPPORTED");
                }
                input.push_back(Json{{"role", role}, {"content", content}});
            }
        }
        for (const auto& call : message.value("tool_calls", Json::array()))
            input.push_back(Json{{"type", "function_call"},
                                 {"call_id", call.at("id")},
                                 {"name", call.at("function").at("name")},
                                 {"arguments", call.at("function").at("arguments")}});
    }
    return input;
}
} // namespace
Json ProviderResult::json() const {
    Json tools = Json::array();
    for (const auto& call : calls)
        tools.push_back(Json{{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    return Json{{"status", status},
                {"response_id", response_id},
                {"resolved_model", resolved_model},
                {"text", text},
                {"tool_calls", tools},
                {"native_output", native_output},
                {"error_code", error_code},
                {"http_status", http_status},
                {"billing_unknown", billing_unknown},
                {"remote_cancel_confirmed", remote_cancel_confirmed},
                {"retry_after_ms", retry_after_ms ? Json(*retry_after_ms) : Json()},
                {"usage",
                 {{"input_tokens", usage.input_tokens ? Json(*usage.input_tokens) : Json()},
                  {"output_tokens", usage.output_tokens ? Json(*usage.output_tokens) : Json()},
                  {"cost_ceiling_micro_usd",
                   usage.cost_ceiling_micro_usd ? Json(*usage.cost_ceiling_micro_usd) : Json()}}}};
}
std::uint64_t provider_cost_ceiling(const ProviderProfile& profile, std::uint64_t input,
                                    std::uint64_t output) {
    if (profile.local && !profile.input_micro_usd_per_million && !profile.output_micro_usd_per_million)
        return 0;
    if (!profile.input_micro_usd_per_million || !profile.output_micro_usd_per_million)
        throw Error("PROVIDER_TARIFF_CEILING_REQUIRED");
    const auto cost = [](std::uint64_t tokens, std::uint64_t rate) {
        if (rate && tokens > (UINT64_MAX - 999999) / rate)
            throw Error("PROVIDER_COST_OVERFLOW");
        return (tokens * rate + 999999) / 1000000;
    };
    const auto a = cost(input, *profile.input_micro_usd_per_million),
               b = cost(output, *profile.output_micro_usd_per_million);
    if (a > UINT64_MAX - b)
        throw Error("PROVIDER_COST_OVERFLOW");
    return a + b;
}
Json provider_payload(const ProviderProfile& profile, const ProviderRequest& request) {
    const auto& budget = request.budget;
    if (!identifier(profile.id) || profile.model.empty() || profile.model.size() > 128 ||
        !profile.context_tokens || profile.context_tokens > 2000000 || !budget.max_output_tokens ||
        budget.max_output_tokens > profile.output_tokens ||
        budget.max_output_tokens > profile.context_tokens ||
        budget.max_output_tokens > budget.max_total_tokens || budget.max_request_bytes > 4 * 1024 * 1024 ||
        !budget.max_response_bytes || budget.max_response_bytes > 16 * 1024 * 1024 ||
        budget.deadline <= Millis(0) || budget.deadline > Millis(300000))
        throw Error("PROVIDER_INVALID_BUDGET_OR_MODEL");
    if (profile.required_vram_bytes > budget.available_vram_bytes)
        throw Error("PROVIDER_VRAM_BUDGET");
    if (!request.tools.is_array() || request.tools.size() > 64 || !request.messages.is_array() ||
        request.messages.size() > 256)
        throw Error("PROVIDER_INPUT_SHAPE_BUDGET");
    if (!request.tools.empty() && !profile.tools)
        throw Error("PROVIDER_TOOLS_UNSUPPORTED");
    if (!request.native_input.is_null() && !request.messages.empty())
        throw Error("PROVIDER_INPUT_PROTOCOL_MIXING");
    if (!profile.vision && !request.native_input.is_null()) {
        std::vector<const Json*> pending{&request.native_input};
        std::size_t visited = 0;
        while (!pending.empty()) {
            const auto* value = pending.back();
            pending.pop_back();
            if (++visited > 131072)
                throw Error("PROVIDER_INPUT_SHAPE_BUDGET");
            if (value->is_object()) {
                const auto type = json_string(*value, "type");
                if (type == "input_image" || type == "image_url")
                    throw Error("PROVIDER_VISION_UNSUPPORTED");
            }
            if (value->is_structured())
                for (const auto& item : *value)
                    pending.push_back(&item);
        }
    }
    if (request.stream && !profile.streaming)
        throw Error("PROVIDER_STREAMING_UNSUPPORTED");
    if (request.background && (!profile.background_cancel || profile.protocol != ProviderProtocol::Responses))
        throw Error("PROVIDER_BACKGROUND_UNSUPPORTED");
    // Reserve against the configured context ceiling, not an optimistic token estimate.
    if (provider_cost_ceiling(profile, profile.context_tokens, budget.max_output_tokens) >
        budget.max_cost_micro_usd)
        throw Error("PROVIDER_COST_BUDGET");
    Json body{{"model", profile.model}, {"stream", request.stream}};
    // Validate message roles and multimodal use consistently for both dialects.
    const auto converted_input = response_input(request.messages, profile.vision);
    if (profile.protocol == ProviderProtocol::Responses) {
        body["input"] = request.native_input.is_null() ? converted_input : request.native_input;
        if (!body["input"].is_array())
            throw Error("PROVIDER_INPUT_SHAPE_BUDGET");
        body["max_output_tokens"] = budget.max_output_tokens;
        body["store"] = request.background;
        body["background"] = request.background;
        body["include"] = Json::array({"reasoning.encrypted_content"});
        body["tools"] = Json::array();
        for (const auto& tool : request.tools) {
            if (json_string(tool, "type") != "function")
                throw Error("PROVIDER_ONLY_DECLARED_FUNCTION_TOOLS");
            auto converted = tool.at("function");
            converted["type"] = "function";
            body["tools"].push_back(std::move(converted));
        }
    } else {
        if (!request.native_input.is_null())
            throw Error("PROVIDER_NATIVE_INPUT_PROTOCOL_MISMATCH");
        for (const auto& message : request.messages)
            if (message.contains("content") && message["content"].is_array() && !profile.vision)
                throw Error("PROVIDER_VISION_UNSUPPORTED");
        body["messages"] = request.messages;
        body["tools"] = request.tools;
        body["max_tokens"] = budget.max_output_tokens;
        if (request.stream)
            body["stream_options"] = Json{{"include_usage", true}};
    }
    const auto bytes = bounded_json_dump(body, budget.max_request_bytes).size();
    // A deliberately conservative byte-based admission bound; measured usage remains separate.
    if (bytes > (UINT64_MAX - 4096) / 4 ||
        bytes * 4 + 4096 > profile.context_tokens - budget.max_output_tokens ||
        bytes * 4 + 4096 > budget.max_total_tokens - budget.max_output_tokens)
        throw Error("PROVIDER_CONTEXT_BUDGET");
    return body;
}
ProviderResult parse_provider_response(ProviderProtocol protocol, const Json& value) {
    ProviderResult result;
    if (!value.is_object())
        throw Error("PROVIDER_RESPONSE_OBJECT_REQUIRED");
    result.response_id = json_string(value, "id");
    result.resolved_model = json_string(value, "model");
    if (protocol == ProviderProtocol::Responses) {
        result.status = json_string(value, "status", "unknown");
        usage(result, value.value("usage", Json()), true);
        if (result.status != "completed") {
            result.billing_unknown = true;
            return result;
        }
        const auto output = value.value("output", Json::array());
        if (!output.is_array() || output.size() > 128)
            throw Error("PROVIDER_OUTPUT_SHAPE_BUDGET");
        for (const auto& item : output) {
            const auto type = json_string(item, "type");
            if (type == "function_call") {
                add_call(result, json_string(item, "call_id"), json_string(item, "name"),
                         json_string(item, "arguments"));
                Json retained{{"type", type},
                              {"call_id", item.at("call_id")},
                              {"name", item.at("name")},
                              {"arguments", item.at("arguments")}};
                if (item.contains("id"))
                    retained["id"] = item["id"];
                result.native_output.push_back(std::move(retained));
            } else if (type == "message") {
                if (json_string(item, "role") != "assistant")
                    throw Error("PROVIDER_OUTPUT_ROLE_ESCALATION");
                Json content_items = Json::array();
                for (const auto& content : item.value("content", Json::array())) {
                    const auto kind = json_string(content, "type");
                    if (kind == "output_text") {
                        result.text += json_string(content, "text");
                        content_items.push_back(Json{{"type", kind}, {"text", json_string(content, "text")}});
                    } else if (kind == "refusal")
                        content_items.push_back(
                            Json{{"type", kind}, {"refusal", json_string(content, "refusal")}});
                    else
                        throw Error("PROVIDER_OUTPUT_CONTENT_UNSUPPORTED");
                }
                Json retained{{"type", type}, {"role", "assistant"}, {"content", content_items}};
                if (item.contains("id"))
                    retained["id"] = item["id"];
                result.native_output.push_back(std::move(retained));
            } else if (type == "reasoning") {
                Json retained{{"type", type}};
                for (const auto* key : {"id", "summary", "encrypted_content"})
                    if (item.contains(key))
                        retained[key] = item[key];
                result.native_output.push_back(std::move(retained));
            } else
                throw Error("PROVIDER_OUTPUT_ITEM_UNSUPPORTED");
        }
    } else {
        const auto& choices = value.at("choices");
        if (!choices.is_array() || choices.size() != 1)
            throw Error("PROVIDER_SINGLE_CHOICE_REQUIRED");
        const auto reason = json_string(choices[0], "finish_reason");
        result.status = reason == "stop" || reason == "tool_calls" ? "completed" : "incomplete";
        usage(result, value.value("usage", Json()), false);
        if (result.status != "completed") {
            result.billing_unknown = true;
            return result;
        }
        const auto& message = choices[0].at("message");
        if (json_string(message, "role", "assistant") != "assistant")
            throw Error("PROVIDER_OUTPUT_ROLE_ESCALATION");
        result.text = json_string(message, "content");
        for (const auto& call : message.value("tool_calls", Json::array())) {
            if (json_string(call, "type") != "function")
                throw Error("PROVIDER_TOOL_TYPE_UNSUPPORTED");
            add_call(result, json_string(call, "id"), json_string(call.at("function"), "name"),
                     json_string(call.at("function"), "arguments"));
        }
    }
    return result;
}
struct ProviderStream::State {
    ProviderProtocol protocol;
    std::size_t maximum, received = 0;
    std::string pending, data;
    std::function<void(std::string_view)> delta;
    bool terminal = false, done = false, finalized = false;
    ProviderResult result;
    struct Call {
        std::string id, name, arguments;
        bool done = false;
    };
    std::map<std::string, Call> calls;
    void event() {
        if (data.empty())
            return;
        if (done)
            throw Error("PROVIDER_EVENT_AFTER_TERMINAL");
        if (data == "[DONE]") {
            done = true;
            data.clear();
            return;
        }
        const auto value = parse_bounded(data, maximum);
        data.clear();
        if (terminal && protocol == ProviderProtocol::Responses)
            throw Error("PROVIDER_EVENT_AFTER_TERMINAL");
        if (protocol == ProviderProtocol::Responses) {
            const auto type = json_string(value, "type");
            if (type == "response.created")
                result.response_id = json_string(value.at("response"), "id");
            else if (type == "response.output_text.delta") {
                const auto text = json_string(value, "delta");
                result.text += text;
                if (delta)
                    delta(text);
            } else if (type == "response.output_item.added" &&
                       json_string(value.at("item"), "type") == "function_call") {
                const auto& item = value.at("item");
                const auto key = json_string(item, "id");
                if (calls.size() >= 32 || !identifier(key) || calls.contains(key))
                    throw Error("PROVIDER_DUPLICATE_TOOL_FRAGMENT");
                calls.emplace(key, Call{json_string(item, "call_id"), json_string(item, "name"),
                                        json_string(item, "arguments"), false});
            } else if (type == "response.function_call_arguments.delta" ||
                       type == "response.function_call_arguments.done") {
                const auto found = calls.find(json_string(value, "item_id"));
                if (found == calls.end() || found->second.done)
                    throw Error("PROVIDER_UNBOUND_TOOL_FRAGMENT");
                if (type.ends_with(".delta"))
                    found->second.arguments += json_string(value, "delta");
                else {
                    const auto final = json_string(value, "arguments");
                    if (!found->second.arguments.empty() && found->second.arguments != final)
                        throw Error("PROVIDER_TOOL_FRAGMENT_MISMATCH");
                    found->second.arguments = final;
                    found->second.done = true;
                }
                if (found->second.arguments.size() > 65536)
                    throw Error("PROVIDER_TOOL_ARGUMENT_BUDGET");
            } else if (type == "response.completed" || type == "response.incomplete" ||
                       type == "response.failed") {
                const auto final = parse_provider_response(protocol, value.at("response"));
                if (!result.response_id.empty() && result.response_id != final.response_id)
                    throw Error("PROVIDER_RESPONSE_ID_CHANGED");
                if (final.status == "completed") {
                    if (!result.text.empty() && result.text != final.text)
                        throw Error("PROVIDER_TEXT_FRAGMENT_MISMATCH");
                    for (const auto& [_, call] : calls) {
                        const auto found = std::find_if(final.calls.begin(), final.calls.end(),
                                                        [&](const auto& item) { return item.id == call.id; });
                        if (found == final.calls.end() || found->name != call.name || !call.done ||
                            canonical_json(found->arguments) != canonical_json(parse_bounded(call.arguments)))
                            throw Error("PROVIDER_INCOMPLETE_TOOL_FRAGMENTS");
                    }
                }
                result = final;
                terminal = true;
            } else if (type == "error")
                throw Error("PROVIDER_STREAM_ERROR");
        } else {
            if (value.contains("usage"))
                usage(result, value["usage"], false);
            if (result.response_id.empty())
                result.response_id = json_string(value, "id");
            else if (!json_string(value, "id").empty() && result.response_id != json_string(value, "id"))
                throw Error("PROVIDER_RESPONSE_ID_CHANGED");
            result.resolved_model = json_string(value, "model", result.resolved_model);
            for (const auto& choice : value.value("choices", Json::array())) {
                if (json_uint(choice, "index") != 0 || terminal)
                    throw Error("PROVIDER_SINGLE_CHOICE_REQUIRED");
                const auto part = choice.value("delta", Json::object());
                const auto text = json_string(part, "content");
                result.text += text;
                if (delta && !text.empty())
                    delta(text);
                for (const auto& tool : part.value("tool_calls", Json::array())) {
                    const auto index = json_uint(tool, "index");
                    if (index >= 32)
                        throw Error("PROVIDER_TOOL_ARGUMENT_BUDGET");
                    auto& call = calls[std::to_string(index)];
                    const auto id = json_string(tool, "id");
                    if (!id.empty()) {
                        if (!call.id.empty() && call.id != id)
                            throw Error("PROVIDER_TOOL_ID_CHANGED");
                        call.id = id;
                    }
                    const auto function = tool.value("function", Json::object());
                    const auto name = json_string(function, "name");
                    if (!name.empty()) {
                        if (!call.name.empty() && call.name != name)
                            throw Error("PROVIDER_TOOL_NAME_CHANGED");
                        call.name = name;
                    }
                    call.arguments += json_string(function, "arguments");
                    if (call.arguments.size() > 65536)
                        throw Error("PROVIDER_TOOL_ARGUMENT_BUDGET");
                }
                const auto reason = json_string(choice, "finish_reason");
                if (!reason.empty()) {
                    result.status = reason == "stop" || reason == "tool_calls" ? "completed" : "incomplete";
                    terminal = true;
                }
            }
        }
    }
};
ProviderStream::ProviderStream(ProviderProtocol protocol, std::size_t maximum,
                               std::function<void(std::string_view)> delta)
    : state_(std::make_unique<State>(
          State{protocol, maximum, 0, {}, {}, std::move(delta), false, false, false, {}, {}})) {
    if (!maximum || maximum > 16 * 1024 * 1024)
        throw Error("PROVIDER_STREAM_BUDGET");
}
ProviderStream::~ProviderStream() = default;
void ProviderStream::feed(std::string_view bytes) {
    auto& state = *state_;
    if (state.finalized)
        throw Error("PROVIDER_STREAM_ALREADY_FINALIZED");
    if (bytes.size() > state.maximum - state.received)
        throw Error("PROVIDER_STREAM_BUDGET");
    state.received += bytes.size();
    state.pending += bytes;
    std::size_t position = 0;
    for (;;) {
        const auto end = state.pending.find('\n', position);
        if (end == std::string::npos)
            break;
        auto line = state.pending.substr(position, end - position);
        position = end + 1;
        if (line.ends_with('\r'))
            line.pop_back();
        if (line.empty())
            state.event();
        else if (line.starts_with("data:")) {
            auto content = line.substr(5);
            if (content.starts_with(' '))
                content.erase(0, 1);
            if (!state.data.empty())
                state.data += '\n';
            state.data += content;
        }
    }
    state.pending.erase(0, position);
}
ProviderResult ProviderStream::finish() {
    auto& state = *state_;
    if (state.finalized)
        return state.result;
    state.finalized = true;
    if (!state.pending.empty() || !state.data.empty() || !state.terminal ||
        (state.protocol == ProviderProtocol::ChatCompletions && !state.done)) {
        state.result.status = "interrupted";
        state.result.calls.clear();
        state.result.billing_unknown = true;
        return state.result;
    }
    if (state.protocol == ProviderProtocol::ChatCompletions && state.result.status == "completed") {
        try {
            auto complete = state.result;
            for (const auto& [_, call] : state.calls)
                add_call(complete, call.id, call.name, call.arguments);
            state.result = std::move(complete);
        } catch (...) {
            state.result.calls.clear();
            state.result.status = "protocol_error";
            state.result.billing_unknown = true;
            throw;
        }
    }
    if (state.result.status != "completed") {
        state.result.calls.clear();
        state.result.billing_unknown = true;
    }
    return state.result;
}
std::string ProviderStream::response_id() const {
    return state_->result.response_id;
}
} // namespace devbox
