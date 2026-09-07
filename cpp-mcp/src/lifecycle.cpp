#include "devbox/lifecycle.hpp"
#include <algorithm>
namespace devbox {
namespace {
bool missing_container(const std::exception& error) {
    auto text = std::string(error.what());
    if (const auto* process = dynamic_cast<const ProcessError*>(&error))
        text += " " + process->stderr_text + " " + process->stdout_text;
    text = lower(text);
    return text.find("no such container") != text.npos || text.find("no such object") != text.npos;
}
bool managed_tmp(const Json& info, const std::string& name) {
    for (const auto& mount : info.value("mounts", Json::array()))
        if (json_string(mount, "Destination") == "/tmp" && json_string(mount, "Type") == "volume" &&
            json_string(mount, "Name") == name)
            return true;
    return false;
}
} // namespace
std::string lifecycle_name(LifecycleAction action) {
    switch (action) {
    case LifecycleAction::start:
        return "start";
    case LifecycleAction::stop:
        return "stop";
    case LifecycleAction::restart:
        return "restart";
    case LifecycleAction::recreate:
        return "recreate";
    }
    throw Error("Unknown lifecycle action");
}
Json host_runtime_info(const Config& config) {
    Json fallback = nullptr;
    if (config.platform.is_windows && trim(env_or("HOST_SHELL", "")).empty() &&
        !trim(config.power_shell_fallback_exe).empty())
        fallback = config.power_shell_fallback_exe;
    return Json{{"mode", "host"},
                {"exists", true},
                {"running", true},
                {"status", "ready"},
                {"name", config.platform.id + "-host-runtime"},
                {"workspacePath", path_text(config.host_workspace_path)},
                {"platform", config.platform.id},
                {"hostDefaultWorkdir", path_text(config.host_default_workdir)},
                {"hostShell", config.host_shell},
                {"hostShellFallback", fallback}};
}
std::vector<std::string> create_container_args(const Config& config) {
    return {"run",
            "-d",
            "--name",
            config.devbox_container_name,
            "--init",
            "-w",
            path_text(config.devbox_workspace_path),
            "-v",
            path_text(config.host_workspace_path) + ":" + path_text(config.devbox_workspace_path),
            "-v",
            config.devbox_tmp_volume_name + ":/tmp",
            config.devbox_image_name,
            "sleep",
            "infinity"};
}
Json parse_docker_info(std::string_view text, std::string_view fallback) {
    const auto data = Json::parse(text);
    const auto nested = [&](const char* object, const char* key) -> Json {
        const auto it = data.find(object);
        if (it == data.end() || !it->is_object())
            return nullptr;
        return it->value(key, Json(nullptr));
    };
    auto name = json_string(data, "Name", std::string(fallback));
    while (name.starts_with('/'))
        name.erase(name.begin());
    auto running = nested("State", "Running"), status = nested("State", "Status");
    return Json{{"exists", true},
                {"id", data.value("Id", Json(nullptr))},
                {"image", nested("Config", "Image")},
                {"running", running.is_boolean() && running.get<bool>()},
                {"status", status.is_string() ? status : Json("unknown")},
                {"startedAt", nested("State", "StartedAt")},
                {"mounts", data.value("Mounts", Json::array())},
                {"name", name}};
}
LifecycleService::LifecycleService(std::shared_ptr<const Config> config, BackgroundTasks& background,
                                   DockerRunner runner)
    : config_(std::move(config)), background_(background), docker_(std::move(runner)) {
    if (!docker_)
        docker_ = [config = config_](const auto& args, const Cancel& cancel) {
            ProcessOptions options;
            options.timeout = Millis(config->docker_command_timeout_ms);
            options.max_capture_chars = 262144;
            return spawn_process("docker", args, options, cancel);
        };
}
Json LifecycleService::inspect(const Cancel& cancel) const {
    try {
        return parse_docker_info(docker_({"inspect", "--type", "container", config_->devbox_container_name,
                                          "--format", "{{json .}}"},
                                         cancel)
                                     .stdout_text,
                                 config_->devbox_container_name);
    } catch (const std::exception& e) {
        if (!missing_container(e))
            throw;
        return Json{{"exists", false},
                    {"name", config_->devbox_container_name},
                    {"running", false},
                    {"status", "missing"}};
    }
}
Json LifecycleService::status(const Cancel& cancel) const {
    return config_->runtime_mode == RuntimeMode::host ? host_runtime_info(*config_) : inspect(cancel);
}
void LifecycleService::set_guardian_desired_state(bool should_run, std::string_view source) const {
    write_json_atomic(config_->project_root / "run" / "guardian.desired-state.json",
                      Json{{"ShouldRun", should_run}, {"UpdatedAtUtc", utc_now()}, {"Source", source}});
}
Json LifecycleService::create(const Cancel& cancel) const {
    docker_(create_container_args(*config_), cancel);
    return inspect(cancel);
}
void LifecycleService::remove(const std::string& name, const Cancel& cancel) const {
    try {
        docker_({"rm", "-f", name}, cancel);
    } catch (const std::exception& e) {
        if (!missing_container(e))
            throw;
    }
}
void LifecycleService::migrate_tmp(const std::string& retired, const Cancel& cancel) const {
    const auto staging = fs::temp_directory_path() / ("docker-chatgpt-devbox-cpp-tmp-" + uuid());
    const auto payload = staging / "payload";
    fs::create_directories(staging);
    try {
        docker_({"cp", retired + ":/tmp", path_text(payload)}, cancel);
        docker_({"cp", path_text(payload) + std::string(1, fs::path::preferred_separator) + ".",
                 config_->devbox_container_name + ":/tmp"},
                cancel);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(staging, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(staging, ec);
}
Json LifecycleService::recreate(const Cancel& cancel) {
    const auto info = inspect(cancel);
    const bool exists = json_bool(info, "exists");
    const bool migrate = exists && !managed_tmp(info, config_->devbox_tmp_volume_name);
    const auto retired =
        config_->devbox_container_name + "-retired-" + std::to_string(unix_millis()) + "-" + uuid();
    if (exists)
        docker_({"rename", config_->devbox_container_name, retired}, cancel);
    Json replacement;
    try {
        replacement = create(cancel);
        if (migrate)
            migrate_tmp(retired, cancel);
    } catch (const std::exception& original) {
        if (!exists)
            throw;
        const auto message = std::string(original.what());
        // Rollback remains possible after the initiating client disconnects.
        auto rollback = std::make_shared<Cancellation>();
        try {
            remove(config_->devbox_container_name, rollback);
            docker_({"rename", retired, config_->devbox_container_name}, rollback);
        } catch (const std::exception& restore) {
            throw Error(message + " Rollback also failed: " + restore.what());
        }
        throw;
    }
    if (exists) {
        const auto runner = docker_;
        background_.once("docker-retired-cleanup", Millis(config_->devbox_retired_container_grace_ms),
                         [runner, retired](const Cancel& cleanup_cancel) {
                             runner({"rm", "-f", retired}, cleanup_cancel);
                         });
    }
    return replacement;
}
Json LifecycleService::control(LifecycleAction action, const Cancel& cancel) {
    std::unique_lock lock(gate_, std::defer_lock);
    while (!lock.try_lock_for(Millis(20)))
        if (cancel)
            cancel->check();
    if (cancel)
        cancel->check();
    if (config_->runtime_mode == RuntimeMode::host) {
        if (action == LifecycleAction::start)
            fs::create_directories(config_->host_workspace_path);
        auto info = host_runtime_info(*config_);
        if (action != LifecycleAction::start) {
            info["controlAction"] = lifecycle_name(action);
            info["controlMessage"] =
                "Host mode runs inside the current server process. Use the devbox launcher command to " +
                lifecycle_name(action) + " the service itself.";
        }
        return info;
    }
    if (action == LifecycleAction::recreate)
        return recreate(cancel);
    auto info = inspect(cancel);
    if (action == LifecycleAction::start) {
        if (!json_bool(info, "exists")) {
            if (!config_->devbox_auto_start)
                throw Error("Devbox container \"" + config_->devbox_container_name +
                            "\" does not exist and DEVBOX_AUTO_START is disabled.");
            return create(cancel);
        }
        if (!json_bool(info, "running"))
            docker_({"start", config_->devbox_container_name}, cancel);
    } else if (action == LifecycleAction::stop) {
        if (!json_bool(info, "exists"))
            return info;
        if (json_bool(info, "running"))
            docker_({"stop", config_->devbox_container_name}, cancel);
    } else {
        if (!json_bool(info, "exists"))
            return create(cancel);
        docker_({"restart", config_->devbox_container_name}, cancel);
    }
    return inspect(cancel);
}
} // namespace devbox
