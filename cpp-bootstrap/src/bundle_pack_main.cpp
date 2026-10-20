#include "devbox/bundle.hpp"
#include "devbox/native.hpp"
#include <iostream>

namespace {
int run(const std::vector<std::string>& args) {
    try {
        if (args.size() == 5 && args[0] == "pack") {
            const auto manifest = devbox::bundle::pack(devbox::path_from_utf8(args[1]),
                                                       devbox::path_from_utf8(args[2]), args[4]);
            devbox::write_json_atomic(devbox::path_from_utf8(args[3]), manifest);
        } else if (args.size() == 4 && args[0] == "attach") {
            devbox::bundle::attach(devbox::path_from_utf8(args[1]), devbox::path_from_utf8(args[2]),
                                   devbox::path_from_utf8(args[3]));
        } else {
            throw devbox::Error("Usage: devbox-bundle-pack pack PACKAGE PAYLOAD METADATA WORKFLOW_RUN\n"
                                "       devbox-bundle-pack attach STUB PAYLOAD OUTPUT");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(devbox::narrow(argv[i]));
    return run(args);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
