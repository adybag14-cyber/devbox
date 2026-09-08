#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include "devbox/setup.hpp"
#include <iostream>
namespace {
int main_impl(const std::vector<std::string>& args) {
    devbox::setup::Options options;
    try {
        options = devbox::setup::parse_options(args);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
    if (options.help) {
        std::cout << devbox::setup::usage();
        return 0;
    }
    if (options.version) {
        std::cout << "devbox-setup " << devbox::setup::installer_version << " (C++)\n";
        return 0;
    }
    try {
        devbox::setup::run(options);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "setup failed: " << e.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(devbox::narrow(argv[i]));
    return main_impl(args);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return main_impl(args);
}
#endif
