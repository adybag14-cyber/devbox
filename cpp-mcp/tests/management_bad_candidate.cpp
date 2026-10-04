#include "devbox/contract.hpp"
#include <iostream>
// Test-only executable: valid reviewed metadata, deliberately fails server startup.
// It is never installed, packaged or exposed through a product fault-injection flag.
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--build-info") {
            std::cout << devbox::build_snapshot().dump() << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--dump-contract") {
            std::cout << devbox::ToolContract(devbox::Config::load()).all().dump() << '\n';
            return 0;
        }
        std::cerr << "intentional qualification fixture startup failure\n";
        return 23;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
