#include "devbox/resource_budget.hpp"
#include <ostream>
#include <streambuf>
namespace devbox {
std::size_t json_memory_charge(const Json& value, std::size_t limit, unsigned depth) {
    if (depth > 64 || limit < 128)
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
class BoundedBuffer final : public std::streambuf {
    std::string output_;
    std::size_t maximum_;
    Cancel cancel_;
    std::streamsize xsputn(const char* text, std::streamsize count) override {
        if (cancel_)
            cancel_->check();
        if (count < 0 || static_cast<std::uint64_t>(count) > maximum_ - output_.size())
            throw ResourceExhausted("RESPONSE_SIZE_BUDGET: result is too large; retrieve bounded pages "
                                    "instead of repeating a side effect");
        output_.append(text, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        const auto byte = traits_type::to_char_type(character);
        xsputn(&byte, 1);
        return character;
    }

  public:
    BoundedBuffer(std::size_t maximum, Cancel cancel) : maximum_(maximum), cancel_(std::move(cancel)) {
        output_.reserve(std::min<std::size_t>(maximum, 65536));
    }
    std::string take() {
        return std::move(output_);
    }
};
} // namespace
std::string bounded_json_dump(const Json& value, std::size_t maximum, const Cancel& cancel) {
    BoundedBuffer buffer(maximum, cancel);
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    // Pin to the resolved nlohmann serializer to preserve its Unicode replacement and numeric
    // formatting exactly. The bounded stream receives small writes without a full temporary dump.
    nlohmann::detail::serializer<Json> serializer(nlohmann::detail::output_adapter<char>(stream), ' ',
                                                  Json::error_handler_t::replace);
    serializer.dump(value, false, false, 0);
    return buffer.take();
}
} // namespace devbox
