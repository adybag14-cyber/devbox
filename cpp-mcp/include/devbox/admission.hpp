#pragma once
#include "config.hpp"
namespace devbox {
Json operator_admission(const Config& config, std::string_view action);
class AdmissionControl {
    fs::path root_;
    mutable std::mutex mutex_;
    Json state_;

  public:
    explicit AdmissionControl(const Config& config);
    void refresh();
    Json snapshot() const;
    bool permits(std::string_view tool, const Json& arguments) const;
};
} // namespace devbox
