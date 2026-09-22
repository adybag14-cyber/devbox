#pragma once
#include "security.hpp"
namespace devbox {
enum class ProviderProtocol { Responses, ChatCompletions };
struct ProviderProfile {
    std::string id, base_url, model;
    ProviderProtocol protocol = ProviderProtocol::Responses;
    bool local = false, tools = false, vision = false, streaming = true, background_cancel = false;
    std::uint64_t context_tokens = 32768, output_tokens = 4096;
    std::uint64_t required_vram_bytes = 0;
    // Operator-configured conservative tariff ceilings, never inferred from a model name.
    std::optional<std::uint64_t> input_micro_usd_per_million, output_micro_usd_per_million;
    std::optional<std::string> secret_reference;
};
struct ProviderBudget {
    std::uint64_t max_output_tokens = 4096, max_total_tokens = 65536, max_cost_micro_usd = 0;
    std::uint64_t available_vram_bytes = 0;
    std::size_t max_request_bytes = 1024 * 1024, max_response_bytes = 4 * 1024 * 1024;
    Millis deadline{60000};
};
struct ProviderRequest {
    // Portable Chat Completions-style message records. Tool output is role=tool, not instructions.
    Json messages = Json::array(), tools = Json::array();
    Json native_input = nullptr;
    ProviderBudget budget;
    bool stream = true, background = false;
};
struct ModelToolCall {
    std::string id, name;
    Json arguments = Json::object();
};
struct ProviderUsage {
    std::optional<std::uint64_t> input_tokens, output_tokens;
    std::optional<std::uint64_t> cost_ceiling_micro_usd;
};
struct ProviderResult {
    std::string status = "unknown", response_id, resolved_model, text, error_code;
    std::vector<ModelToolCall> calls;
    Json native_output = Json::array();
    ProviderUsage usage;
    bool billing_unknown = true, remote_cancel_confirmed = false;
    int http_status = 0;
    std::optional<std::uint64_t> retry_after_ms;
    Json json() const;
};
class ProviderStream {
    struct State;
    std::unique_ptr<State> state_;

  public:
    ProviderStream(ProviderProtocol protocol, std::size_t max_bytes = 4 * 1024 * 1024,
                   std::function<void(std::string_view)> text_delta = {});
    ~ProviderStream();
    void feed(std::string_view bytes);
    ProviderResult finish();
    std::string response_id() const;
};
Json provider_payload(const ProviderProfile& profile, const ProviderRequest& request);
ProviderResult parse_provider_response(ProviderProtocol protocol, const Json& response);
std::uint64_t provider_cost_ceiling(const ProviderProfile& profile, std::uint64_t input,
                                    std::uint64_t output);
class ModelProvider {
    ProviderProfile profile_;
    SecretBroker* secrets_;

  public:
    explicit ModelProvider(ProviderProfile profile, SecretBroker* secrets = nullptr);
    Json capabilities() const;
    ProviderResult generate(const ProviderRequest& request, const SecretScope& scope,
                            const Cancel& cancel = {}, std::function<void(std::string_view)> text_delta = {});
    ProviderResult cancel_background(std::string_view response_id, const SecretScope& scope,
                                     const Cancel& cancel = {});
    ProviderResult retrieve(std::string_view response_id, const SecretScope& scope,
                            const Cancel& cancel = {});
};
} // namespace devbox
