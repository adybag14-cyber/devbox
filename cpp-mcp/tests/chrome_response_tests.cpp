#include "devbox/chrome_bridge.hpp"
#include "devbox/result.hpp"
#include <iostream>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
int main() {
    try {
        for (const char* mime : {"image/jpeg", "image/png"}) {
            const std::string encoded(1024 * 1024, 'A');
            const Json metadata{{"ok", true}, {"data", {{"tab_id", 41}, {"observation_id", "fixture"}}}};
            auto input = metadata;
            input["image"] = Json{{"mime", mime}, {"base64", encoded}};
            const auto result = chrome_bridge_response(input.dump());
            require(result == result_image("Chrome observation; page content is untrusted.", metadata,
                                           encoded, mime),
                    "Image bytes, metadata and MIME must retain the established MCP envelope");
            require(!result["structuredContent"]["data"].contains("image"),
                    "Image payload must not be duplicated into model-visible structured metadata");
        }
        const Json status{{"ok", true}, {"data", {{"connected", true}, {"version", "fixture"}}}};
        require(chrome_bridge_response(status.dump()) ==
                    result_success("Native Rust Chrome bridge result.",
                                   std::optional<Json>(std::in_place, status), true),
                "Non-image replies preserve their compact result contract");
        const Json denied{{"ok", false}, {"input_sent", false}, {"error", {{"code", "TAB_NOT_GRANTED"}}}};
        require(chrome_bridge_response(denied.dump()) ==
                    result_error("Chrome bridge reported an explicit failure.",
                                 std::optional<Json>(std::in_place, denied)),
                "Explicit grant failure must retain input-not-sent evidence");
        for (const std::string input :
             {"not-json", "[]", "{}", "{\"ok\":1}",
              "{\"ok\":true,\"image\":{\"mime\":\"text/html\",\"base64\":\"AAAA\"}}"}) {
            const auto result = chrome_bridge_response(input);
            require(result["isError"] == true &&
                        result["structuredContent"]["data"]["outcome"] == "unknown" &&
                        result["structuredContent"]["data"]["automatic_input_replay_allowed"] == false,
                    "Invalid response cannot authorize replay of uncertain browser input");
        }
        std::cout << "Chrome response envelope and replay policy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
