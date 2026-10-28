#include "devbox/allocator.hpp"
#include "devbox/common.hpp"
#include <iostream>

using namespace devbox;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class Fn> void rejects(Fn action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Lookup changed its established wrong-type rejection");
}
} // namespace
int main() {
    try {
        const std::vector<std::string> keys{
            "", "short", std::string(128, 'k'), "utf8-\xc3\xa9-\xf0\x9f\x98\x80",
            std::string("diagnostic\0key", sizeof("diagnostic\0key") - 1) + "-exact-tail"};
        for (const auto& key : keys) {
            const std::string padded = "prefix-" + key + "-suffix";
            const std::string_view view(padded.data() + 7, key.size());
            Json value = Json::object();
            value[key] = "ready";
            require(json_string(value, view) == "ready", "Bounded string-view key did not match exactly");
            value[key] = true;
            require(json_bool(value, view), "Boolean view lookup failed");
            rejects([&] { (void)json_string(value, view); });
            value[key] = 42;
            require(json_uint(value, view) == 42 && json_number(value, view) == 42,
                    "Integer view lookup failed");
            value[key] = 1.5;
            require(json_number(value, view) == 1.5, "Floating-point view lookup failed");
            rejects([&] { (void)json_uint(value, view); });
            value[key] = -1;
            rejects([&] { (void)json_uint(value, view); });
            value[key] = nullptr;
            require(json_string(value, view, "fallback") == "fallback" && json_bool(value, view, true) &&
                        json_uint(value, view, 7) == 7 && json_number(value, view, 2.5) == 2.5,
                    "Explicit null must preserve each fallback");
            value.erase(key);
            require(json_string(value, view, "missing") == "missing" && !json_bool(value, view) &&
                        json_uint(value, view, 9) == 9 && json_number(value, view, 3.5) == 3.5 &&
                        value.empty(),
                    "Missing lookup must use fallback without inserting a field");
        }
        for (const auto& scalar : Json::array({nullptr, true, 42, 1.5, "text", Json::array()}))
            require(json_string(scalar, "key", "fallback") == "fallback" &&
                        json_uint(scalar, "key", 3) == 3 && json_bool(scalar, "key", true) &&
                        json_number(scalar, "key", 4.5) == 4.5,
                    "Non-object lookup changed its fallback behavior");

        const std::string key(96, 'k');
        const std::string_view view(key);
        const Json text{{key, "ready"}}, number{{key, 42}}, flag{{key, true}}, fraction{{key, 1.5}};
        std::uint64_t observed = 0;
        const auto before = allocator_counters();
        for (unsigned n = 0; n < 2000; ++n) {
            observed += json_string(text, view).size();
            observed += json_uint(number, view);
            observed += json_bool(flag, view);
            observed += json_number(fraction, view) == 1.5;
        }
        const auto after = allocator_counters();
        require(observed == 98000, "The bounded lookup fixture returned incorrect values");
        std::cout << "lookup_allocation_calls=" << after.calls - before.calls << '\n';
        require(after.calls == before.calls, "Successful long-key lookups allocate temporary key storage");
        std::cout << "JSON subviews, empty/UTF-8/NUL keys, defaults, types and allocation checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
