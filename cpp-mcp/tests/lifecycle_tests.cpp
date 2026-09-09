#include "devbox/docker_files.hpp"
#include "devbox/lifecycle.hpp"
#include <iostream>
#include <map>
#include <thread>
using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
template <class F> void until(F predicate, const char* message) {
    const auto deadline = Clock::now() + Millis(2000);
    while (!predicate() && Clock::now() < deadline)
        std::this_thread::sleep_for(Millis(5));
    require(predicate(), message);
}
struct DockerFixture {
    std::mutex mutex;
    std::map<std::string, Json> containers;
    std::vector<std::vector<std::string>> calls;
    bool fail_create = false, fail_migration = false;
    Json info(std::string name, bool managed = false) const {
        return Json{
            {"Id", uuid()},
            {"Name", "/" + name},
            {"Config", {{"Image", "fixture-image"}}},
            {"State", {{"Running", true}, {"Status", "running"}, {"StartedAt", "2026-01-01T00:00:00Z"}}},
            {"Mounts",
             managed
                 ? Json::array({Json{{"Destination", "/tmp"}, {"Type", "volume"}, {"Name", "fixture-tmp"}}})
                 : Json::array()}};
    }
    ProcessOutput run(const std::vector<std::string>& args, const Cancel& cancel) {
        if (cancel)
            cancel->check();
        std::lock_guard lock(mutex);
        calls.push_back(args);
        ProcessOutput result;
        if (args[0] == "inspect") {
            const auto found = containers.find(args[3]);
            if (found == containers.end()) {
                ProcessError error("docker inspect failed");
                error.stderr_text = "No such container";
                error.exit_code = 1;
                throw error;
            }
            result.stdout_text = found->second.dump();
        } else if (args[0] == "rename") {
            auto node = containers.extract(args[1]);
            require(!node.empty(), "rename source exists");
            node.key() = args[2];
            node.mapped()["Name"] = "/" + args[2];
            containers.insert(std::move(node));
        } else if (args[0] == "run") {
            if (fail_create)
                throw Error("replacement creation failure");
            containers[args[3]] = info(args[3], true);
        } else if (args[0] == "rm")
            containers.erase(args[2]);
        else if (args[0] == "cp") {
            if (fail_migration)
                throw Error("temporary file migration failure");
        } else if (args[0] == "start" || args[0] == "restart" || args[0] == "stop") {
            auto& value = containers.at(args[1]);
            const bool running = args[0] != "stop";
            value["State"]["Running"] = running;
            value["State"]["Status"] = running ? "running" : "exited";
        } else
            throw Error("Unexpected fixture Docker operation");
        return result;
    }
};
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-cpp-lifecycle-" + uuid());
    try {
        DockerFixture docker;
        auto config = std::make_shared<Config>();
        config->project_root = root;
        config->platform = Platform::detect();
        config->host_workspace_path = root / "workspace";
        config->host_default_workdir = config->host_workspace_path;
        config->host_shell = "fixture";
        BackgroundTasks background;
        LifecycleService host(config, background);
        require(host.status()["running"] == true, "host runtime status");
        host.control(LifecycleAction::start);
        require(fs::is_directory(config->host_workspace_path), "host start workspace");
        require(host.control(LifecycleAction::stop)["controlAction"] == "stop" && process_alive(process_id()),
                "host stop never kills MCP process");
        host.set_guardian_desired_state(false, "fixture");
        require(read_json(root / "run/guardian.desired-state.json")["ShouldRun"] == false,
                "Guardian desired state contract");
        auto docker_config = std::make_shared<Config>(*config);
        docker_config->runtime_mode = RuntimeMode::docker;
        docker_config->devbox_container_name = "fixture-box";
        docker_config->devbox_image_name = "fixture-image";
        docker_config->devbox_tmp_volume_name = "fixture-tmp";
        docker_config->devbox_workspace_path = "/workspace";
        docker_config->devbox_auto_start = false;
        docker_config->devbox_retired_container_grace_ms = 25;
        auto runner = [&](const auto& args, const Cancel& cancel) { return docker.run(args, cancel); };
        LifecycleService lifecycle(docker_config, background, runner);
        require(lifecycle.status()["exists"] == false, "missing Docker runtime");
        bool refused = false;
        try {
            lifecycle.control(LifecycleAction::start);
        } catch (const Error& e) {
            refused = std::string(e.what()).find("AUTO_START") != std::string::npos;
        }
        require(refused, "disabled auto start honored");
        docker.containers["fixture-box"] = docker.info("fixture-box");
        const auto original = docker.containers["fixture-box"]["Id"];
        require(lifecycle.control(LifecycleAction::stop)["running"] == false, "Docker stop");
        require(lifecycle.control(LifecycleAction::restart)["running"] == true, "Docker restart");
        docker.fail_create = true;
        bool create_failed = false;
        try {
            lifecycle.control(LifecycleAction::recreate);
        } catch (const Error& e) {
            create_failed = std::string(e.what()).find("creation failure") != std::string::npos;
        }
        require(create_failed && docker.containers.size() == 1 &&
                    docker.containers["fixture-box"]["Id"] == original,
                "failed replacement rolls back original runtime");
        docker.fail_create = false;
        docker.fail_migration = true;
        bool migration_failed = false;
        try {
            lifecycle.control(LifecycleAction::recreate);
        } catch (const Error& e) {
            migration_failed = std::string(e.what()).find("migration failure") != std::string::npos;
        }
        require(migration_failed && docker.containers.size() == 1 &&
                    docker.containers["fixture-box"]["Id"] == original,
                "failed tmp migration rolls back original runtime");
        docker.fail_migration = false;
        const auto replaced = lifecycle.control(LifecycleAction::recreate);
        require(replaced["id"] != original && replaced["running"] == true, "Docker replacement created");
        until(
            [&] {
                std::lock_guard lock(docker.mutex);
                return docker.containers.size() == 1;
            },
            "retired runtime removed after grace period");
        until(
            [&] {
                const auto value = background.snapshot();
                return value.contains("docker-retired-cleanup") &&
                       value["docker-retired-cleanup"]["running"] == false;
            },
            "retired cleanup has terminal tracking");
        const auto cleanup = background.snapshot()["docker-retired-cleanup"];
        require(cleanup["consecutiveFailures"] == 0 && cleanup["lastSuccessUnixMs"].is_number(),
                "cleanup records real success");
        require(normalize_posix_path("/workspace", "a/../b.txt") == "/workspace/b.txt",
                "container path locks normalize aliases");
        require(normalize_posix_path("/workspace", "/tmp/./x") == "/tmp/x",
                "absolute container path normalization");
        std::atomic_size_t attempts{0};
        background.periodic("recovery-fixture", Millis(0), Millis(20), [&](const Cancel&) {
            if (++attempts == 1)
                throw Error("fixture transient failure");
        });
        until(
            [&] {
                const auto value = background.snapshot();
                return attempts >= 2 && value["recovery-fixture"]["consecutiveFailures"] == 0;
            },
            "periodic failure recovers");
        const auto recovered = background.snapshot()["recovery-fixture"];
        require(recovered["lastFailureUnixMs"].is_number() && recovered["lastSuccessUnixMs"].is_number(),
                "attempt/failure/success telemetry remains distinct");
        background.mark_started("idle-events");
        const auto idle = background.snapshot()["idle-events"];
        require(idle["idleForMs"].is_number() && idle["lastSuccessUnixMs"].is_null(),
                "event-driven idle is not fabricated success");
        background.mark_stopped("idle-events");
        background.stop();
        require(background.snapshot()["recovery-fixture"]["running"] == false,
                "background shutdown records stopped state");
        fs::remove_all(root);
        std::cout << "Lifecycle rollback, container arguments, Guardian state and background recovery checks "
                     "passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nFixture: " << root << '\n';
        return 1;
    }
}
