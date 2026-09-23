#include "devbox/wsl.hpp"
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F&& work) {
    try {
        work();
    } catch (const Error&) {
        return;
    }
    throw Error("Expected structured WSL rejection");
}
} // namespace
int main() {
    try {
        const std::string literal = "Unicode \xce\xa9 \xf0\x9f\x98\x80 $() & ; ' \"\nnext line";
        Json request{{"action", "run"},         {"distribution", "Ubuntu"},
                     {"working_dir", "/tmp"},   {"program", "/usr/bin/printf"},
                     {"args", {"%s", literal}}, {"timeout_seconds", 3}};
        const auto arguments = wsl_program_arguments(request);
        require(arguments.back() == literal &&
                    std::find(arguments.begin(), arguments.end(), "--exec") != arguments.end() &&
                    std::find(arguments.begin(), arguments.end(), "-i") != arguments.end(),
                "literal argv and clean guest environment");
        auto invalid = request;
        invalid["program"] = "printf";
        rejects([&] { wsl_program_arguments(invalid); });
        invalid = request;
        invalid["working_dir"] = "C:\\Users";
        rejects([&] { wsl_program_arguments(invalid); });
        invalid = request;
        invalid["distribution"] = "--shutdown";
        rejects([&] { wsl_program_arguments(invalid); });
        invalid = request;
        invalid["args"] = {std::string("a\0b", 3)};
        rejects([&] { wsl_program_arguments(invalid); });
        Config config;
        config.platform = Platform::detect();
        require(wsl_capabilities(config)["status"] ==
                    (config.platform.is_windows ? "permission_denied" : "unsupported"),
                "platform and operator-policy failures remain distinct");
        if (const auto selected = environment("DEVBOX_WSL_TEST_DISTRIBUTION")) {
#ifdef _WIN32
            config.host_exec_enabled = true;
            const auto listed = wsl_operation(config, Json{{"action", "distributions"}});
            require(listed["status"] == "available" &&
                        std::find(listed["distributions"].begin(), listed["distributions"].end(),
                                  Json(*selected)) != listed["distributions"].end(),
                    "explicit registered WSL distribution found");
            request["distribution"] = *selected;
            const auto result = wsl_operation(config, request);
            require(result["status"] == "completed" && result["stdout"] == literal &&
                        result["exit_code"] == 0,
                    "actual WSL literal Unicode/metacharacter roundtrip");
            request["program"] = "/bin/cat";
            request["args"] = Json::array();
            request["input"] = literal;
            require(wsl_operation(config, request)["stdout"] == literal, "actual WSL stdin roundtrip");
            request.erase("input");
            request["program"] = "/bin/sleep";
            request["args"] = {"3"};
            request["timeout_seconds"] = 1;
            require(wsl_operation(config, request)["status"] == "guest_deadline",
                    "guest deadline is distinguished from a bridge failure");
            const auto mapped = wsl_operation(config, Json{{"action", "map_path"},
                                                           {"distribution", *selected},
                                                           {"direction", "to_linux"},
                                                           {"path", "C:\\Users\\adyba"}});
            require(json_string(mapped, "mapped_path").starts_with('/'), "distribution-owned path mapping");
            std::cout << "Actual WSL distribution, argv, stdin, mapping and timeout passed\n";
#else
            throw Error("Actual WSL qualification requires Windows");
#endif
        }
        std::cout << "Structured WSL arguments and explicit platform states passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
