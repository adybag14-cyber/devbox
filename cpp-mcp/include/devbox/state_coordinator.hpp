#pragma once
#include "state_store.hpp"
namespace devbox {
struct StateClientOptions {
    fs::path executable;
    Millis timeout{2000};
    bool start_if_absent = true;
    Cancel cancel;
    // Native qualification comparator, not an environment or MCP option. Writes stay fresh.
    bool reuse_read_connections = true;
};
// Authenticated loopback IPC; credentials stay in the private state directory, never argv/env.
std::shared_ptr<StateStore> open_coordinated_state(const fs::path& directory,
                                                   StateClientOptions options = {});
int run_state_coordinator(const fs::path& directory, const std::function<bool()>& stop_requested);
bool stop_state_coordinator(const fs::path& directory, Millis wait = Millis(5000));
} // namespace devbox
