#pragma once
#include "config.hpp"
#include <set>
namespace devbox {
Json build_snapshot();
std::string build_version();
class ParameterError : public Error {
  public:
    using Error::Error;
};
class ToolContract {
    Json tools_;

  public:
    explicit ToolContract(const Config& config);
    const Json& all() const {
        return tools_;
    }
    Json selected(const std::set<std::string>& implemented) const;
    const Json& tool(std::string_view name) const;
    Json arguments(std::string_view name, const Json& supplied) const;
    Json capabilities(const Config& config, const std::set<std::string>& implemented) const;
};
} // namespace devbox
