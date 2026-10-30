#pragma once
#include "config.hpp"
#include "process.hpp"
namespace devbox {
Json chrome_bridge_call(const Config& config, const Json& args, const Cancel& cancel = {});
}
