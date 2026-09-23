#pragma once
#include "jobs.hpp"
#include "runs.hpp"
namespace devbox {
class RunService {
    std::shared_ptr<const Config> config_;
    JobStore jobs_;
    Json profile(std::string_view id) const;
    std::shared_ptr<StateStore> state() const;
    int drive_impl(std::string_view principal, std::string_view run_id, const std::function<bool()>& stop);

  public:
    explicit RunService(std::shared_ptr<const Config> config) : config_(config), jobs_(std::move(config)) {}
    Json profiles() const;
    Json call(std::string_view principal, const Json& arguments);
    Json start_driver(std::string_view principal, std::string_view run_id);
    void recover(const Cancel& cancel = {});
    int drive(std::string_view principal, std::string_view run_id, const std::function<bool()>& stop = {});
};
} // namespace devbox
