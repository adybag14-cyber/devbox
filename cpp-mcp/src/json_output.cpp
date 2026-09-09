#include "devbox/common.hpp"
#include <algorithm>

namespace devbox {
namespace {
void append_string(std::string& output, const std::string& text, Json::error_handler_t errors) {
    // Printable ASCII needs neither escaping nor UTF-8 repair. Larger strings
    // can be copied directly after validation; all other encodings retain the
    // reference serializer's exact escaping and error policy.
    unsigned invalid = 0;
    for (unsigned char c : text)
        invalid |= static_cast<unsigned>(c < 0x20) | static_cast<unsigned>(c >= 0x80) |
                   static_cast<unsigned>(c == '"') | static_cast<unsigned>(c == '\\');
    if (!invalid) {
        output += '"';
        output += text;
        output += '"';
    } else
        output += Json(text).dump(-1, ' ', false, errors);
}
void append_value(std::string& output, const Json& value, Json::error_handler_t errors) {
    if (value.is_string())
        append_string(output, value.get_ref<const std::string&>(), errors);
    else if (value.is_array()) {
        output += '[';
        bool first = true;
        for (const auto& item : value) {
            if (!first)
                output += ',';
            first = false;
            append_value(output, item, errors);
        }
        output += ']';
    } else if (value.is_object()) {
        output += '{';
        bool first = true;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!first)
                output += ',';
            first = false;
            append_string(output, it.key(), errors);
            output += ':';
            append_value(output, it.value(), errors);
        }
        output += '}';
    } else
        output += value.dump(-1, ' ', false, errors);
}
} // namespace
std::string json_dump(const Json& value, Json::error_handler_t errors) {
    std::string output;
    output.reserve(1024);
    append_value(output, value, errors);
    return output;
}
} // namespace devbox
