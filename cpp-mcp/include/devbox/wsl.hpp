#pragma once
#include "config.hpp"
#include "process.hpp"
namespace devbox {
Json wsl_capabilities(const Config& config);
Json wsl_operation(const Config& config, const Json& args, const Cancel& cancel = {});
std::vector<std::string> wsl_program_arguments(const Json& args);
} // namespace devbox
