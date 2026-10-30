#include "devbox/resource_budget.hpp"
#include <memory>
namespace devbox {
std::size_t json_memory_charge(const Json& value, std::size_t limit, unsigned depth) {
    // The request parser accepts 128 levels; result envelopes add several more.
    if (depth > 256 || limit < 128)
        return limit + 1;
    std::size_t bytes = 128;
    if (value.is_string() || value.is_binary()) {
        const auto length =
            value.is_string() ? value.get_ref<const std::string&>().size() : value.get_binary().size();
        return length > (limit - bytes) / 16 ? limit + 1 : bytes + length * 16;
    }
    if (value.is_structured())
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (value.is_object()) {
                if (it.key().size() > (limit - bytes) / 16)
                    return limit + 1;
                bytes += it.key().size() * 16;
            }
            const auto child = json_memory_charge(*it, limit - bytes, depth + 1);
            if (child > limit - bytes)
                return limit + 1;
            bytes += child;
        }
    return bytes;
}
namespace {
class BoundedOutput final : public nlohmann::detail::output_adapter_protocol<char> {
    std::string output_;
    std::size_t maximum_;
    Cancel cancel_;
    void append(const char* text, std::size_t count) {
        if (cancel_)
            cancel_->check();
        if (count > maximum_ - output_.size())
            throw ResourceExhausted("RESPONSE_SIZE_BUDGET: result is too large; retrieve bounded pages "
                                    "instead of repeating a side effect");
        output_.append(text, count);
    }

  public:
    BoundedOutput(std::size_t maximum, Cancel cancel) : maximum_(maximum), cancel_(std::move(cancel)) {
        output_.reserve(std::min<std::size_t>(maximum, 65536));
    }
    void write_character(char character) override {
        append(&character, 1);
    }
    void write_characters(const char* text, std::size_t count) override {
        append(text, count);
    }
    std::string take() {
        return std::move(output_);
    }
};
} // namespace
std::string bounded_json_dump(const Json& value, std::size_t maximum, const Cancel& cancel) {
    auto output = std::make_shared<BoundedOutput>(maximum, cancel);
    // Pin to the resolved nlohmann serializer to preserve its Unicode replacement and numeric
    // formatting exactly. The adapter bounds every write and checks cancellation,
    // avoiding ostream sentries and streambuf forwarding for each punctuation byte.
    nlohmann::detail::serializer<Json> serializer(output, ' ', Json::error_handler_t::replace);
    serializer.dump(value, false, false, 0);
    return output->take();
}
} // namespace devbox
