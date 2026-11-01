#pragma once
#include "config.hpp"
#include "process.hpp"
namespace devbox {
Json chrome_bridge_call(const Config& config, const Json& args, const Cancel& cancel = {});
// Pure response formatting. Platform, executable identity and transport policy
// are enforced by chrome_bridge_call before this function receives any bytes.
Json chrome_bridge_response(std::string output);
} // namespace devbox
