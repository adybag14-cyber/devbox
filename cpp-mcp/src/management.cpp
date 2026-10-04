#include "devbox/management.hpp"
#include "devbox/admission.hpp"
#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include "devbox/scoped_thread.hpp"
#include "devbox/state_coordinator.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <future>
#include <iostream>
#include <thread>
#ifdef __APPLE__
#include <libproc.h>
#endif

namespace devbox {
namespace {
#ifdef _WIN32
volatile LONG stop_signal = 0;
void signal_stop(int) {
    InterlockedExchange(&stop_signal, 1);
}
bool take_stop_signal() {
    return InterlockedExchange(&stop_signal, 0) != 0;
}
BOOL WINAPI console_stop(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_SHUTDOWN_EVENT) {
        signal_stop(0);
        return TRUE;
    }
    return FALSE;
}
#else
volatile std::sig_atomic_t stop_signal = 0;
void signal_stop(int) {
    stop_signal = 1;
}
bool take_stop_signal() {
    const bool value = stop_signal != 0;
    stop_signal = 0;
    return value;
}
#endif
fs::path control_root(const fs::path& root) {
    return root / "run" / "native";
}
std::string executable_name() {
#ifdef _WIN32
    return "devbox-mcp.exe";
#else
    return "devbox-mcp";
#endif
}
Json config_read(const fs::path& root) {
    auto result = read_json(control_root(root) / "config.json", 1024 * 1024);
    if (json_uint(result, "schema") != 1 || json_string(result, "root") != path_text(root) ||
        !result.at("environment").is_object())
        throw Error("NATIVE_MANAGEMENT_CONFIG_INVALID");
    return result;
}
void config_write(const fs::path& root, const Json& config) {
    write_json_atomic(control_root(root) / "config.json", config);
}
void log(const fs::path& root, std::string_view value) {
    static std::mutex mutex;
    std::lock_guard guard(mutex);
    const auto file = control_root(root) / "supervisor.log";
    // Serialized by the supervisor; no unbounded output or global log directory.
    if (fs::exists(file) && fs::file_size(file) >= 1024 * 1024) {
        for (unsigned index = 3; index > 0; --index) {
            const auto from =
                index == 1 ? file : path_from_utf8(path_text(file) + "." + std::to_string(index - 1));
            const auto to = path_from_utf8(path_text(file) + "." + std::to_string(index));
            if (fs::exists(from))
                replace_state_file(from, to);
        }
    }
    write_file(file, utc_now() + " " + std::string(value.substr(0, 8192)) + "\n", true);
}
bool identity_alive(const Json& record) {
    const auto pid = json_uint(record, "pid"), birth = management_instance(record);
    return pid > 0 && pid <= UINT32_MAX && birth > 0 &&
           process_matches_instance(static_cast<std::uint32_t>(pid), birth);
}
bool process_image_matches(std::uint32_t pid, const fs::path& expected) {
    try {
#ifdef _WIN32
        NativeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process)
            return false;
        std::vector<wchar_t> text(32768);
        DWORD size = static_cast<DWORD>(text.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, text.data(), &size))
            return false;
        return fs::equivalent(fs::path(std::wstring(text.data(), size)), expected);
#elif defined(__linux__)
        return fs::equivalent(fs::read_symlink("/proc/" + std::to_string(pid) + "/exe"), expected);
#elif defined(__APPLE__)
        char text[PROC_PIDPATHINFO_MAXSIZE]{};
        return proc_pidpath(static_cast<int>(pid), text, sizeof(text)) > 0 &&
               fs::equivalent(fs::path(text), expected);
#else
        (void)pid;
        (void)expected;
        return false;
#endif
    } catch (...) {
        return false;
    }
}
std::string local_url(const Environment& env) {
    auto host = env.at("HOST");
    if (host == "0.0.0.0" || host.empty())
        host = "127.0.0.1";
    if (host == "::" || host == "[::]")
        host = "::1";
    if (host.find(':') != host.npos && !host.starts_with('['))
        host = "[" + host + "]";
    return "http://" + host + ":" + env.at("PORT");
}
Json metadata(const Environment& env) {
    const auto response =
        http_request("GET", local_url(env) + "/", {}, Json::object(), Millis(1000), 256 * 1024, {}, true);
    if (response.status != 200)
        throw Error("NATIVE_LOCAL_METADATA_UNAVAILABLE");
    return Json::parse(response.body);
}
bool matches_server(const Json& value, const Json& candidate, std::string_view generation) {
    if (!value.contains("build"))
        return false;
    return json_string(value.at("build"), "deploymentGeneration") == generation &&
           json_string(value.at("build"), "binarySha256") == json_string(candidate, "sha256");
}
bool ready(const Environment& env, const Json& candidate, std::string_view generation) {
    try {
        if (!matches_server(metadata(env), candidate, generation))
            return false;
        const auto response =
            http_request("GET", local_url(env) + "/readyz", {}, Json::object(), Millis(1000), 1024, {}, true);
        return response.status == 200 && json_bool(Json::parse(response.body), "ok");
    } catch (...) {
        return false;
    }
}
void verify_stored(const fs::path& root, const Json& candidate, std::string_view policy) {
    const auto file = path_from_utf8(candidate.at("file").get<std::string>());
    const auto hash = json_string(candidate, "sha256");
    const auto expected = control_root(root) / "releases" / hash / executable_name();
    if (hash.size() != 64 || file != expected || fs::canonical(file) != expected ||
        sha256_file(file) != hash ||
        (policy == "signed-qualified" && json_string(candidate, "policy") != policy))
        throw Error("NATIVE_STORED_CANDIDATE_IDENTITY_MISMATCH");
    const auto& build = candidate.at("build");
    if (json_uint(build, "stateSchemaVersion") != state_store_schema_version ||
        json_uint(build, "stateCoordinatorProtocol") != 1 ||
        json_uint(build, "contractVersion") != cpp_contract_version)
        throw Error("NATIVE_STATE_OR_CONTRACT_UPGRADE_REQUIRES_REVIEW");
}
Json prepare_candidate(ManagementOptions options, const Json& config) {
    // Copy first; all proof verification and execution operate on the private immutable copy.
    if (options.binary.empty())
        options.binary = executable_path();
    const auto source = fs::canonical(options.binary);
    const auto hash = sha256_file(source, 256 * 1024 * 1024);
    const auto directory = control_root(options.root) / "releases" / hash;
    ensure_private_state_directory(directory.parent_path());
    ensure_private_state_directory(directory);
    const auto destination = directory / executable_name();
    if (!fs::exists(destination)) {
        const auto staged = directory / ("candidate-" + uuid());
        ScopeExit cleanup([&] {
            std::error_code ec;
            fs::remove(staged, ec);
        });
        fs::copy_file(source, staged, fs::copy_options::none);
        if (sha256_file(staged) != hash)
            throw Error("NATIVE_CANDIDATE_COPY_MISMATCH");
#ifndef _WIN32
        fs::permissions(staged, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
#endif
        fs::rename(staged, destination);
    }
    if (sha256_file(destination) != hash)
        throw Error("NATIVE_CANDIDATE_TAMPERED");
    options.binary = destination;
    // External downloads are copied into a private verification directory so
    // later changes to the original receipt/bundle cannot change the decision.
    const auto proof = directory / ("proof-" + uuid());
    ensure_private_state_directory(proof);
    ScopeExit remove_proof([&] {
        std::error_code ec;
        fs::remove_all(proof, ec);
    });
    if (!options.allow_local_build && !options.receipt.empty() && !options.bundle.empty()) {
        if (fs::file_size(options.receipt) > 8 * 1024 * 1024 ||
            fs::file_size(options.bundle) > 16 * 1024 * 1024)
            throw Error("NATIVE_QUALIFICATION_INPUT_TOO_LARGE");
        fs::copy_file(options.receipt, proof / "receipt.json");
        fs::copy_file(options.bundle, proof / "bundle.json");
        options.receipt = proof / "receipt.json";
        options.bundle = proof / "bundle.json";
    }
    const auto qualified = qualify_managed_binary(options, managed_environment(options.root, config));
    write_json_atomic(directory / "verification.json", qualified);
    return qualified;
}
ProcessOutput binary_command(const fs::path& root, const Json& config, const Json& candidate,
                             const std::vector<std::string>& arguments, Millis timeout = Millis(5000)) {
    verify_stored(root, candidate, json_string(config, "policy"));
    ProcessOptions options;
    options.cwd = root;
    options.env = managed_environment(root, config);
    options.timeout = timeout;
    options.max_capture_chars = 65536;
    return spawn_process(json_string(candidate, "file"), arguments, options);
}
Json admission(const fs::path& root, const Json& config, const Json& candidate, std::string_view action) {
    return Json::parse(
        binary_command(root, config, candidate, {"--admission", std::string(action)}).stdout_text);
}
void resume_admission(const fs::path& root, const Json& config, const Json& candidate,
                      std::string_view generation = {}) {
    const auto requested = admission(root, config, candidate, "resume").at("requested");
    if (generation.empty())
        return;
    const auto deadline = Clock::now() + Millis(5000);
    const auto env = managed_environment(root, config, std::string(generation));
    do {
        try {
            const auto value = metadata(env);
            if (matches_server(value, candidate, generation) &&
                value.at("admission").at("generation") == requested.at("generation") &&
                json_string(value.at("admission"), "mode") == "open")
                return;
        } catch (...) {
        }
        std::this_thread::sleep_for(Millis(100));
    } while (Clock::now() < deadline);
    throw Error("NATIVE_ADMISSION_RESUME_UNCONFIRMED");
}
bool durable_idle(const fs::path& root) {
    const auto directory = root / "run" / "state";
    if (!fs::exists(directory / "metadata.sqlite3"))
        return true;
    StateStoreOptions options;
    options.writable = false;
    const auto store = open_state_store(directory, options);
    const auto generation = store->generation();
    for (const auto* kind : {"job", "run"}) {
        const auto total = store->count(kind);
        std::uint64_t terminal = 0;
        const auto statuses =
            std::string_view(kind) == "job"
                ? std::vector<std::string>{"succeeded", "failed", "cancelled", "timed_out", "interrupted"}
                : std::vector<std::string>{"completed", "failed", "cancelled"};
        for (const auto& status : statuses)
            terminal += store->count(kind, {}, status);
        if (total != terminal)
            return false;
    }
    return store->generation() == generation;
}
struct Child {
    Clock::time_point started = Clock::now();
    std::future<void> task;
    Cancel cancel = std::make_shared<Cancellation>();
    std::atomic<std::uint32_t> pid{0};
    std::optional<std::uint64_t> instance;
    bool adopted = false;
    std::string generation;
    ~Child() {
        if (task.valid()) {
            cancel->cancel();
            task.wait();
        }
    }
    bool alive() const {
        return pid && instance && process_matches_instance(pid, instance);
    }
    Json identity() const {
        return Json{{"pid", pid.load()},
                    {"instance", instance ? Json(std::to_string(*instance)) : Json()},
                    {"generation", generation}};
    }
    void stop() {
        if (adopted) {
            if (alive() && !terminate_process_tree(pid, instance))
                throw Error("NATIVE_OWNED_STOP_UNCONFIRMED");
        } else if (task.valid()) {
            cancel->cancel();
            if (task.wait_for(Millis(10000)) != std::future_status::ready)
                throw Error("NATIVE_CHILD_STOP_TIMED_OUT");
            task.get();
        }
        const auto deadline = Clock::now() + Millis(5000);
        while (alive() && Clock::now() < deadline)
            std::this_thread::sleep_for(Millis(25));
        if (alive())
            throw Error("NATIVE_OWNED_STOP_UNCONFIRMED");
    }
};
std::unique_ptr<Child> launch_owned(std::string file, std::vector<std::string> arguments,
                                    ProcessOptions options, std::string generation = {}) {
    auto child = std::make_unique<Child>();
    child->generation = generation.empty() ? uuid() : std::move(generation);
    auto* state = child.get();
    options.on_pid = [state](std::uint32_t pid) { state->pid.store(pid); };
    child->task = std::async(
        std::launch::async, [state, options, file = std::move(file), arguments = std::move(arguments)] {
            try {
                (void)spawn_process(file, arguments, options, state->cancel);
            } catch (const std::exception&) { /* Exit is observed by the owner. Capture remains bounded. */
            }
        });
    const auto deadline = Clock::now() + Millis(5000);
    while (!child->pid && Clock::now() < deadline &&
           child->task.wait_for(Millis(10)) != std::future_status::ready) {
    }
    child->instance = process_instance(child->pid);
    if (!child->alive())
        throw Error("NATIVE_CHILD_FAILED_TO_START");
    return child;
}
std::unique_ptr<Child> launch(const fs::path& root, const Json& config, const Json& candidate) {
    verify_stored(root, candidate, json_string(config, "policy"));
    const auto generation = uuid();
    ProcessOptions options;
    options.cwd = root;
    options.env = managed_environment(root, config, generation);
    options.max_capture_chars = 16384;
    options.allow_durable_children = true;
    options.on_output = [root](OutputStream stream, std::string_view text) {
        log(root, std::string(stream == OutputStream::stderr_stream ? "mcp stderr: " : "mcp stdout: ") +
                      std::string(text));
    };
    return launch_owned(json_string(candidate, "file"), {}, std::move(options), generation);
}
// Optional operator-configured sidecars (for example cloudflared). A failed
// sidecar is repaired independently and cannot cause a healthy MCP restart.
class Companions {
    struct Entry {
        Json spec;
        std::unique_ptr<Child> child;
        Clock::time_point retry = Clock::now();
        unsigned failures = 0;
        unsigned unhealthy_probes = 0;
        std::string error;
    };
    fs::path root_;
    Environment environment_;
    std::vector<Entry> entries_;

  public:
    Companions(const fs::path& root, const Json& config, const Json& prior)
        : root_(root), environment_(managed_environment(root, config)) {
        for (const auto& spec : config.value("companions", Json::array())) {
            Entry entry;
            entry.spec = spec;
            const auto name = json_string(spec, "name");
            for (const auto& old : prior.value("companions", Json::array())) {
                if (json_string(old, "name") != name || !old.contains("process") ||
                    !identity_alive(old.at("process")))
                    continue;
                const auto& identity = old.at("process");
                const auto pid = static_cast<std::uint32_t>(json_uint(identity, "pid"));
                const auto file = path_from_utf8(json_string(spec, "program"));
                if (!process_image_matches(pid, file) || sha256_file(file) != json_string(spec, "sha256") ||
                    !identity_alive(identity))
                    throw Error("NATIVE_COMPANION_ORPHAN_REQUIRES_REVIEW: " + name);
                entry.child = std::make_unique<Child>();
                entry.child->pid = pid;
                entry.child->instance = management_instance(identity);
                entry.child->generation = json_string(identity, "generation");
                entry.child->adopted = true;
            }
            entries_.push_back(std::move(entry));
        }
    }
    ~Companions() {
        try {
            stop();
        } catch (...) {
        }
    }
    void stop() {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (it->child) {
                it->child->stop();
                it->child.reset();
            }
    }
    Json tick() {
        Json value = Json::array();
        bool prerequisites = true;
        for (auto& entry : entries_) {
            const auto name = json_string(entry.spec, "name");
            if (entry.child && !entry.child->alive()) {
                entry.child->stop();
                entry.child.reset();
                ++entry.failures;
                entry.retry = Clock::now() + Millis(std::min(60000U, 1000U << std::min(entry.failures, 6U)));
                entry.error = "owned companion exited";
            }
            if (!prerequisites && entry.child) {
                entry.child->stop();
                entry.child.reset();
            }
            if (prerequisites && (!entry.child || !entry.child->alive()) && Clock::now() >= entry.retry) {
                try {
                    if (entry.child)
                        entry.child->stop();
                    entry.child.reset();
                    const auto file = path_from_utf8(json_string(entry.spec, "program"));
                    if (fs::canonical(file) != file || sha256_file(file) != json_string(entry.spec, "sha256"))
                        throw Error("COMPANION_BINARY_CHANGED");
                    ProcessOptions options;
                    options.cwd = root_;
                    options.env = environment_;
                    const auto configured = entry.spec.value("environment", Json::object());
                    for (const auto& [key, data] : configured.items())
                        options.env->insert_or_assign(key, data.get<std::string>());
                    options.max_capture_chars = 4096;
                    options.on_output = [this, name](OutputStream, std::string_view data) {
                        log(root_, name + ": " + std::string(data));
                    };
                    entry.child = launch_owned(path_text(file), json_strings(entry.spec, "args"), options);
                    entry.error.clear();
                } catch (const std::exception& error) {
                    entry.error = error.what();
                    ++entry.failures;
                    entry.retry =
                        Clock::now() + Millis(std::min(60000U, 1000U << std::min(entry.failures, 6U)));
                }
            }
            bool healthy = entry.child && entry.child->alive();
            if (healthy && entry.spec.contains("readinessUrl")) {
                try {
                    healthy = http_request("GET", json_string(entry.spec, "readinessUrl"), {}, Json::object(),
                                           Millis(1000), 4096, {}, true)
                                  .status == 200;
                } catch (...) {
                    healthy = false;
                }
            }
            if (healthy && Clock::now() - entry.child->started >= Millis(30000))
                entry.failures = 0;
            entry.unhealthy_probes = healthy ? 0 : entry.unhealthy_probes + 1;
            if (!healthy && entry.child && entry.child->alive() && entry.unhealthy_probes >= 3) {
                entry.child->stop();
                entry.child.reset();
                ++entry.failures;
                entry.retry = Clock::now() + Millis(std::min(60000U, 1000U << std::min(entry.failures, 6U)));
                entry.error = "companion readiness failed repeatedly";
                entry.unhealthy_probes = 0;
            }
            value.push_back(Json{{"name", name},
                                 {"healthy", healthy},
                                 {"error", entry.error},
                                 {"process", entry.child ? entry.child->identity() : Json()}});
            // Explicit ordering permits a display/tunnel prerequisite without
            // invoking a shell or a second orchestration runtime.
            if (json_bool(entry.spec, "requiredBeforeNext"))
                prerequisites = healthy;
        }
        return value;
    }
};
void wait_ready(const fs::path& root, const Json& config, const Json& candidate, const Child& child,
                Millis timeout) {
    const auto deadline = Clock::now() + timeout;
    const auto env = managed_environment(root, config, child.generation);
    do {
        if (!child.alive())
            throw Error("NATIVE_CHILD_EXITED_BEFORE_READY");
        if (ready(env, candidate, child.generation))
            return;
        std::this_thread::sleep_for(Millis(100));
    } while (Clock::now() < deadline);
    throw Error("NATIVE_CHILD_READINESS_TIMEOUT");
}
bool drain(const fs::path& root, const Json& config, const Json& candidate, const Child& child,
           Millis timeout) {
    const auto prior = admission(root, config, candidate, "status");
    const auto requested = admission(root, config, candidate, "drain").at("requested");
    bool finished = false;
    ScopeExit restore([&] {
        if (!finished && json_string(prior, "mode") == "open") {
            try {
                resume_admission(root, config, candidate, child.alive() ? child.generation : "");
            } catch (...) {
            }
        }
    });
    const auto env = managed_environment(root, config, child.generation);
    const auto deadline = Clock::now() + timeout;
    unsigned observations = 0;
    do {
        bool idle = false;
        try {
            if (child.alive()) {
                const auto value = metadata(env);
                const auto& activity = value.at("activity");
                idle = matches_server(value, candidate, child.generation) &&
                       value.at("admission").at("generation") == requested.at("generation") &&
                       json_string(value.at("admission"), "mode") == "draining" &&
                       activity.at("activeTools") == 0 && activity.at("activeRequests") == 0 &&
                       durable_idle(root);
            } else
                idle = durable_idle(root);
        } catch (...) {
            idle = false;
        }
        observations = idle ? observations + 1 : 0;
        if (observations >= 2) {
            finished = true;
            return json_string(prior, "mode") == "open";
        }
        std::this_thread::sleep_for(Millis(100));
    } while (Clock::now() < deadline);
    throw Error("NATIVE_DRAIN_BUSY_OR_UNCONFIRMED: existing work was preserved");
}
Json management_status(const fs::path& root) {
    auto value = read_json_optional(control_root(root) / "status.json", 65536).value_or(Json::object());
    value["running"] = value.contains("owner") && identity_alive(value.at("owner"));
    const auto heartbeat =
        read_json_optional(control_root(root) / "heartbeat.json", 65536).value_or(Json::object());
    const auto observed = json_uint(heartbeat, "unixMs");
    value["heartbeatFresh"] = observed > 0 && observed <= unix_millis() + 5000 &&
                              observed + 15000 >= unix_millis() &&
                              json_string(heartbeat, "epoch") == json_string(value, "epoch");
    if (!value["running"].get<bool>() || !json_bool(value, "heartbeatFresh") || !value.contains("child") ||
        !identity_alive(value.at("child")))
        value["healthy"] = false;
    value["nativeManagement"] = true;
    return value;
}
int supervise(const ManagementOptions& options) {
    const auto root = options.root, dir = control_root(root);
    FileLock owner_lock(dir / "owner.lock", Millis(100), {}, true);
    auto config = config_read(root);
    if (!json_bool(config, "desired", true))
        return 0;
    const auto prior_status = read_json_optional(dir / "status.json", 65536).value_or(Json::object());
    Companions companions(root, config, prior_status);
    Json current = config.at("current");
    auto handoff = read_json_optional(dir / "handoff.json", 1024 * 1024);
    if (handoff) {
        verify_stored(root, handoff->at("previous"), json_string(config, "policy"));
        verify_stored(root, handoff->at("candidate"), json_string(config, "policy"));
    }
    const auto epoch = uuid();
    const auto owner_instance = process_instance(process_id());
    if (!owner_instance)
        throw Error("NATIVE_OWNER_IDENTITY_UNAVAILABLE");
    Json state{{"schema", 1},
               {"epoch", epoch},
               {"owner", {{"pid", process_id()}, {"instance", std::to_string(*owner_instance)}}},
               {"healthy", false},
               {"phase", "starting"},
               {"restarts", 0},
               {"lastRequest", nullptr}};
    std::unique_ptr<Child> child;
    if (prior_status.contains("child") && identity_alive(prior_status.at("child"))) {
        const auto& previous = prior_status.at("child");
        verify_stored(root, current, json_string(config, "policy"));
        const auto observed = metadata(managed_environment(root, config));
        if (!matches_server(observed, current, json_string(previous, "generation"))) {
            if (!handoff ||
                !matches_server(observed, handoff->at("candidate"), json_string(previous, "generation")))
                throw Error("NATIVE_ORPHAN_IDENTITY_UNCONFIRMED: refusing duplicate startup");
            current = handoff->at("candidate");
            config["previous"] = handoff->at("previous");
            config["current"] = current;
            config_write(root, config);
        }
        if (!process_image_matches(static_cast<std::uint32_t>(json_uint(previous, "pid")),
                                   path_from_utf8(json_string(current, "file"))) ||
            !identity_alive(previous))
            throw Error("NATIVE_ORPHAN_EXECUTABLE_IDENTITY_MISMATCH");
        child = std::make_unique<Child>();
        child->pid = static_cast<std::uint32_t>(json_uint(previous, "pid"));
        child->instance = management_instance(previous);
        child->generation = json_string(previous, "generation");
        child->adopted = true;
    }
    auto publish = [&] {
        state["updatedAt"] = utc_now();
        state["candidateSha256"] = current.at("sha256");
        state["child"] = child ? child->identity() : Json();
        write_json_atomic(dir / "status.json", state);
    };
    const auto complete_handoff = [&] {
        if (!handoff || !child || !child->alive())
            return;
        if (json_bool(*handoff, "resume"))
            resume_admission(root, config, current, child->generation);
        fs::remove(dir / "handoff.json");
        handoff.reset();
    };
    publish();
    // Heartbeat ownership stays observable during slow probes or bounded drains.
    const auto heartbeat_stop = std::make_shared<Cancellation>();
    ScopedThread heartbeat([dir, epoch, owner = state.at("owner"), heartbeat_stop] {
        while (!heartbeat_stop->cancelled()) {
            try {
                write_json_atomic(dir / "heartbeat.json", Json{{"epoch", epoch},
                                                               {"owner", owner},
                                                               {"unixMs", unix_millis()},
                                                               {"updatedAt", utc_now()}});
            } catch (...) { /* Readers report a stale heartbeat rather than success. */
            }
            (void)heartbeat_stop->wait_for(Millis(5000));
        }
    });
    ScopeExit stop_heartbeat([&] { heartbeat_stop->cancel(); });
    (void)take_stop_signal();
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_stop, TRUE);
#endif
    std::string last_request;
    unsigned failures = 0, unhealthy = 0;
    auto next_start = Clock::now();
    auto next_health = Clock::now();
    log(root, "native supervisor started epoch=" + epoch);
    for (;;) {
        if (child && !child->alive()) {
            child->stop();
            child.reset();
            ++failures;
            next_start = Clock::now() + Millis(std::min(60000U, 1000U << std::min(failures, 6U)));
            state["healthy"] = false;
            state["phase"] = "backoff";
            state["restarts"] = json_uint(state, "restarts") + 1;
            log(root, "owned frontend exited; restart is subject to backoff");
            publish();
        }
        if ((!child || !child->alive()) && Clock::now() >= next_start) {
            try {
                if (child)
                    child->stop();
                child = launch(root, config, current);
                state["phase"] = "starting";
                publish();
                wait_ready(root, config, current, *child, options.timeout);
                complete_handoff();
                unhealthy = 0;
                state["phase"] = "ready";
                state["healthy"] = true;
                state.erase("error");
            } catch (const std::exception& error) {
                if (child)
                    child->stop();
                child.reset();
                state["healthy"] = false;
                state["phase"] = "backoff";
                state["error"] = error.what();
                if (config.contains("previous") &&
                    config.at("previous").at("sha256") != current.at("sha256")) {
                    verify_stored(root, config.at("previous"), json_string(config, "policy"));
                    current = config.at("previous");
                    config["current"] = current;
                    config.erase("previous");
                    config_write(root, config);
                    log(root, "startup failed; previous immutable candidate restored");
                }
                ++failures;
                next_start = Clock::now() + Millis(std::min(60000U, 1000U << std::min(failures, 6U)));
                state["restarts"] = json_uint(state, "restarts") + 1;
                log(root, error.what());
            }
            publish();
        }
        Json request = Json::object();
        try {
            request = read_json_optional(dir / "request.json", 1024 * 1024).value_or(Json::object());
        } catch (const std::exception& error) {
            state["controlError"] = error.what();
        }
        if (take_stop_signal())
            request = Json{{"epoch", epoch}, {"id", uuid()}, {"action", "stop"}};
        if (json_string(request, "epoch") == epoch && !json_string(request, "id").empty() &&
            json_string(request, "id") != last_request) {
            last_request = json_string(request, "id");
            const auto action = json_string(request, "action");
            const auto operation_timeout = Millis(std::clamp<std::uint64_t>(
                json_uint(request, "timeoutMs", static_cast<std::uint64_t>(options.timeout.count())), 1,
                300000));
            const bool replacing = action == "promote" || action == "rollback";
            Json acknowledgement{{"id", last_request}, {"ok", false}};
            try {
                if (action != "stop" && action != "restart" && !replacing)
                    throw Error("NATIVE_UNKNOWN_CONTROL_ACTION");
                const auto proposed = replacing ? request.at("candidate") : current;
                verify_stored(root, proposed, json_string(config, "policy"));
                state["phase"] = "draining";
                publish();
                bool resume = true;
                if (child)
                    resume = drain(root, config, current, *child, operation_timeout);
                else if (!durable_idle(root))
                    throw Error("NATIVE_DURABLE_WORK_ACTIVE");
                handoff = Json{{"previous", current},
                               {"candidate", proposed},
                               {"resume", resume},
                               {"action", action},
                               {"request", last_request}};
                write_json_atomic(dir / "handoff.json", *handoff);
                if (child)
                    child->stop();
                child.reset();
                // The old coordinator can outlive the frontend. Release its ownership only
                // after the frontend acknowledged drain and durable work was reconciled.
                if (!stop_state_coordinator(root / "run" / "state"))
                    throw Error("NATIVE_COORDINATOR_STOP_UNCONFIRMED");
                if (action == "stop") {
                    companions.stop();
                    config["desired"] = false;
                    config_write(root, config);
                    acknowledgement["ok"] = true;
                    state["lastRequest"] = acknowledgement;
                    state["phase"] = "stopped";
                    state["healthy"] = false;
                    if (resume)
                        (void)admission(root, config, current, "resume");
                    fs::remove(dir / "handoff.json");
                    handoff.reset();
                    publish();
                    log(root, "native supervisor stopped after drain");
                    return 0;
                }
                const auto previous = current;
                try {
                    child = launch(root, config, proposed);
                    state["phase"] = "starting-candidate";
                    state["healthy"] = false;
                    publish();
                    wait_ready(root, config, proposed, *child, operation_timeout);
                    if (resume)
                        resume_admission(root, config, proposed, child->generation);
                    current = proposed;
                    if (replacing)
                        config["previous"] = previous;
                    config["current"] = current;
                    config_write(root, config);
                    fs::remove(dir / "handoff.json");
                    handoff.reset();
                    acknowledgement["ok"] = true;
                } catch (const std::exception& error) {
                    if (child)
                        child->stop();
                    child.reset();
                    if (!stop_state_coordinator(root / "run" / "state"))
                        throw Error("NATIVE_ROLLBACK_COORDINATOR_BUSY");
                    child = launch(root, config, previous);
                    wait_ready(root, config, previous, *child, operation_timeout);
                    if (resume)
                        resume_admission(root, config, previous, child->generation);
                    fs::remove(dir / "handoff.json");
                    handoff.reset();
                    acknowledgement["rolledBack"] = true;
                    acknowledgement["error"] = error.what();
                    log(root, "candidate failed; previous immutable binary restored");
                }
                state["phase"] = "ready";
                state["healthy"] = true;
            } catch (const std::exception& error) {
                acknowledgement["error"] = error.what();
                state["phase"] = "control-refused";
                log(root, error.what());
            }
            state["lastRequest"] = acknowledgement;
            publish();
        }
        if (child && child->alive() && Clock::now() >= next_health) {
            state["companions"] = companions.tick();
            const auto env = managed_environment(root, config, child->generation);
            const bool healthy = ready(env, current, child->generation);
            if (healthy && Clock::now() - child->started >= Millis(30000))
                failures = 0;
            if (healthy)
                complete_handoff();
            state["healthy"] = healthy;
            unhealthy = healthy ? 0 : unhealthy + 1;
            // Public route failures are diagnostic only; they never authorize killing MCP.
            if (config.at("environment").contains("PUBLIC_BASE_URL")) {
                const auto url = json_string(config.at("environment"), "PUBLIC_BASE_URL");
                if (!url.empty())
                    try {
                        state["publicHealthy"] =
                            http_request("GET", url + "/readyz", {}, Json::object(), Millis(1500), 4096)
                                .status == 200;
                    } catch (...) {
                        state["publicHealthy"] = false;
                    }
            }
            if (unhealthy >= 3) {
                state["phase"] = "unhealthy";
                // Repair only a positively identified, drained frontend. Busy or unknown
                // state is reported without interrupting work or restarting unrelated services.
                try {
                    const bool resume = drain(root, config, current, *child, Millis(2000));
                    child->stop();
                    child.reset();
                    if (resume)
                        (void)admission(root, config, current, "resume");
                    next_start = Clock::now();
                } catch (const std::exception& error) {
                    state["error"] = error.what();
                }
                unhealthy = 0;
            }
            next_health = Clock::now() + Millis(5000);
            publish();
        }
        std::this_thread::sleep_for(Millis(200));
    }
}
Json send_control(const ManagementOptions& options, std::string action, const Json& candidate = Json()) {
    const auto observed = management_status(options.root);
    if (!json_bool(observed, "running"))
        throw Error("NATIVE_SUPERVISOR_NOT_RUNNING");
    const auto id = uuid();
    Json request{{"id", id}, {"action", action}, {"epoch", observed.at("epoch")}, {"candidate", candidate}};
    if (options.timeout_set)
        request["timeoutMs"] = options.timeout.count();
    write_json_atomic(control_root(options.root) / "request.json", request);
    const auto deadline = Clock::now() + options.timeout * 3 + Millis(15000);
    do {
        const auto value = management_status(options.root);
        if (value.contains("lastRequest") && value.at("lastRequest").is_object() &&
            json_string(value.at("lastRequest"), "id") == id) {
            if (!json_bool(value.at("lastRequest"), "ok"))
                throw Error(value.at("lastRequest").dump());
            return value;
        }
        if (!json_bool(value, "running"))
            throw Error("NATIVE_SUPERVISOR_EXITED_DURING_CONTROL");
        std::this_thread::sleep_for(Millis(100));
    } while (Clock::now() < deadline);
    throw Error("NATIVE_CONTROL_TIMEOUT: inspect status before retrying");
}
} // namespace

std::uint64_t management_instance(const Json& identity) {
    if (!identity.contains("instance"))
        return 0;
    const auto& value = identity.at("instance");
    if (!value.is_string())
        return json_uint(identity, "instance");
    const auto text = value.get<std::string>();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0;
}
Environment managed_environment(const fs::path& root, const Json& config, std::string generation) {
    auto env = worker_environment();
    // No ambient project configuration, loader variables, or production state paths.
    for (auto it = env.begin(); it != env.end();) {
        if (it->first.starts_with("DEVBOX_") || it->first.starts_with("MCP_") ||
            it->first.starts_with("HOST_"))
            it = env.erase(it);
        else
            ++it;
    }
    env["HOST"] = "127.0.0.1";
    env["PORT"] = "8100";
    env["MCP_AUTH_MODE"] = "none";
    env["DEVBOX_RUNTIME_MODE"] = "host";
    env["HOST_WORKSPACE_PATH"] = path_text(root / "workspace");
    env["HOST_DEFAULT_WORKDIR"] = path_text(root / "workspace");
    env["DEVBOX_WORKSPACE_PATH"] = path_text(root / "workspace");
    for (const auto& [key, value] : config.at("environment").items())
        env[key] = value.get<std::string>();
    env["DEVBOX_PROJECT_ROOT"] = path_text(root);
    env["DEVBOX_NATIVE_MANAGED"] = "1";
    env["DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE"] = "1";
    env["DEVBOX_DEPLOYMENT_GENERATION"] = generation;
    env["MCP_STATE_BACKEND"] = "sqlite";
    env["MCP_STATE_ROOT"] = path_text(root / "run" / "state");
    env["MCP_JOBS_ROOT"] = path_text(root / "run" / "jobs");
    env["MCP_EXEC_SLOT_ROOT"] = path_text(root / "run" / "slots");
    env["MCP_PERFORMANCE_STATE_PATH"] = path_text(root / "run" / "mcp-performance.json");
    return env;
}
ManagementOptions parse_management_options(const std::vector<std::string>& args) {
    ManagementOptions result;
    if (!args.empty())
        result.command = args.front();
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& key = args[index];
        if (key == "--allow-local-build") {
            result.allow_local_build = true;
            continue;
        }
        if (index + 1 >= args.size())
            throw Error("Missing value for " + key);
        const auto& value = args[++index];
        if (key == "--root")
            result.root = path_from_utf8(value);
        else if (key == "--binary")
            result.binary = path_from_utf8(value);
        else if (key == "--receipt")
            result.receipt = path_from_utf8(value);
        else if (key == "--provenance")
            result.bundle = path_from_utf8(value);
        else if (key == "--source")
            result.source = value;
        else if (key == "--target")
            result.target = value;
        else if (key == "--env-file")
            result.env_file = path_from_utf8(value);
        else if (key == "--companions")
            result.companions = path_from_utf8(value);
        else if (key == "--service")
            result.service = value;
        else if (key == "--output")
            result.output = path_from_utf8(value);
        else if (key == "--port" || key == "--timeout-ms") {
            unsigned number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number < 1 ||
                number > (key == "--port" ? 65535U : 300000U))
                throw Error("Invalid " + key);
            if (key == "--port") {
                result.port = number;
                result.port_set = true;
            } else {
                result.timeout = Millis(number);
                result.timeout_set = true;
            }
        } else
            throw Error("Unknown native management option " + key);
    }
    if (result.command != "help" && result.command != "--help") {
        if (result.root.empty() || !result.root.is_absolute())
            throw Error("Native management requires an absolute --root PATH");
        result.root = fs::weakly_canonical(result.root);
        if (result.root == result.root.root_path())
            throw Error("A filesystem root cannot be a managed installation");
    }
    return result;
}
int management_main(const std::vector<std::string>& args) {
    const auto options = parse_management_options(args);
    const auto& root = options.root;
    const auto& command = options.command;
    if (command == "help" || command == "--help") {
        std::cout
            << "Native C++23 management (no Node/npm/Git runtime)\n"
               "devbox-mcp manage init|start|run|status|stop|restart|promote|rollback|service-file --root "
               "ABSOLUTE_PATH\n"
               "init/promote: --binary FILE --receipt FILE --provenance FILE --source SHA --target TARGET\n"
               "Development init/promote only: --allow-local-build (never downgrades a signed installation)\n"
               "init: --port NUMBER --env-file FILE; service-file: --service systemd|launchd|windows "
               "--output FILE\n"
               "--timeout-ms NUMBER bounds startup/drain. Native state stays under root/run.\n";
        return 0;
    }
    if (command == "init") {
        if (fs::exists(root) && !fs::is_empty(root))
            throw Error("Native init requires a new empty root; production migration is separate");
        ensure_directory(root.parent_path());
        const auto staging = root.parent_path() / (".devbox-init-" + uuid());
        ensure_private_state_directory(staging);
        ScopeExit discard_staging([&] {
            std::error_code ec;
            fs::remove_all(staging, ec);
        });
        ensure_private_state_directory(staging / "run");
        ensure_private_state_directory(control_root(staging));
        ensure_private_state_directory(staging / "workspace");
        Json config{{"schema", 1},
                    {"root", path_text(root)},
                    {"desired", true},
                    {"policy", options.allow_local_build ? "local-development" : "signed-qualified"},
                    {"environment", Json{{"PORT", std::to_string(options.port)}}}};
        if (!options.env_file.empty()) {
            const auto values = parse_env_text(read_file(options.env_file, 1024 * 1024));
            for (const auto& [key, value] : values.items())
                config["environment"][key] = value;
        }
        if (options.port_set)
            config["environment"]["PORT"] = std::to_string(options.port);
        config["companions"] = Json::array();
        if (!options.companions.empty()) {
            auto values = read_json(options.companions, 65536);
            if (!values.is_array() || values.size() > 8)
                throw Error("At most eight native companions may be configured");
            std::vector<std::string> names;
            for (auto& spec : values) {
                const auto name = json_string(spec, "name");
                validate_key(name);
                if (std::find(names.begin(), names.end(), name) != names.end())
                    throw Error("Duplicate companion name");
                names.push_back(name);
                const auto file = path_from_utf8(json_string(spec, "program"));
                if (!file.is_absolute())
                    throw Error("Companion program must be absolute");
                spec["program"] = path_text(fs::canonical(file));
                spec["sha256"] = sha256_file(file);
                if (!spec.contains("args"))
                    spec["args"] = Json::array();
                if (!spec["args"].is_array() || spec["args"].size() > 128)
                    throw Error("Companion arguments must be a bounded array");
                for (const auto& argument : spec["args"])
                    if (!argument.is_string())
                        throw Error("Companion arguments must be strings");
                if (spec.contains("readinessUrl")) {
                    const auto url = Url::parse(json_string(spec, "readinessUrl"));
                    if (url.scheme != "http" ||
                        (url.host != "127.0.0.1" && url.host != "[::1]" && url.host != "::1"))
                        throw Error("Companion readiness must use a numeric HTTP loopback address");
                }
            }
            config["companions"] = std::move(values);
        }
        auto staged_options = options;
        staged_options.root = staging;
        config["current"] = prepare_candidate(staged_options, config);
        const auto release = fs::path("releases") / json_string(config.at("current"), "sha256");
        config["current"]["file"] = path_text(control_root(root) / release / executable_name());
        write_json_atomic(control_root(staging) / release / "verification.json", config.at("current"));
        config_write(staging, config);
        // Publish only a fully verified configuration. Failed proof leaves the
        // requested destination empty/absent and can be retried without cleanup.
        if (fs::exists(root)) {
            if (!fs::is_empty(root) || !fs::remove(root))
                throw Error("Native destination changed during initialization");
        }
        fs::rename(staging, root);
        std::cout << Json{{"initialized", true},
                          {"root", path_text(root)},
                          {"policy", config.at("policy")},
                          {"sha256", config.at("current").at("sha256")}}
                         .dump()
                  << '\n';
        return 0;
    }
    if (command == "status") {
        std::cout << management_status(root).dump(2) << '\n';
        return 0;
    }
    (void)config_read(root);
    ensure_private_state_directory(control_root(root));
    if (command == "run" || command == "supervise")
        return supervise(options);
    FileLock lock(control_root(root) / "operator.lock", options.timeout, {}, true);
    auto config = config_read(root);
    Json result;
    if (command == "status")
        result = management_status(root);
    else if (command == "service-file") {
        if (options.output.empty())
            throw Error("service-file requires --output FILE");
        const auto value = native_service_definition(options.service, fs::canonical(executable_path()), root);
        atomic_write(options.output, value, false, true, Preconditions{"missing", {}});
        result = Json{{"written", path_text(fs::absolute(options.output))}, {"activated", false}};
    } else if (command == "start") {
        result = management_status(root);
        const auto prior_stop_deadline = Clock::now() + Millis(5000);
        while (json_bool(result, "running") && json_string(result, "phase") == "stopped" &&
               Clock::now() < prior_stop_deadline) {
            std::this_thread::sleep_for(Millis(50));
            result = management_status(root);
        }
        if (!json_bool(result, "running")) {
            config["desired"] = true;
            config_write(root, config);
            std::optional<std::uint64_t> instance;
            const auto pid = spawn_detached(executable_path(),
                                            {"manage", "supervise", "--root", path_text(root), "--timeout-ms",
                                             std::to_string(options.timeout.count())},
                                            root, worker_environment(), &instance);
            const auto deadline = Clock::now() + options.timeout + Millis(5000);
            do {
                result = management_status(root);
                if (json_bool(result, "running") && json_bool(result, "healthy"))
                    break;
                if (!instance || !process_matches_instance(pid, instance))
                    throw Error("NATIVE_SUPERVISOR_START_FAILED");
                std::this_thread::sleep_for(Millis(100));
            } while (Clock::now() < deadline);
            if (!json_bool(result, "healthy"))
                throw Error("NATIVE_START_TIMEOUT: inspect native status and logs");
        }
        if (!json_bool(result, "healthy")) {
            const auto deadline = Clock::now() + options.timeout;
            do {
                std::this_thread::sleep_for(Millis(100));
                result = management_status(root);
                if (!json_bool(result, "running"))
                    throw Error("NATIVE_EXISTING_SUPERVISOR_EXITED");
            } while (!json_bool(result, "healthy") && Clock::now() < deadline);
            if (!json_bool(result, "healthy"))
                throw Error("NATIVE_EXISTING_SUPERVISOR_UNHEALTHY");
        }
    } else if (command == "stop" || command == "restart")
        result = send_control(options, command);
    else if (command == "promote" || command == "rollback") {
        if (options.allow_local_build && json_string(config, "policy") != "local-development")
            throw Error("NATIVE_QUALIFICATION_POLICY_DOWNGRADE_REFUSED");
        const auto candidate =
            command == "rollback" ? config.at("previous") : prepare_candidate(options, config);
        verify_stored(root, candidate, json_string(config, "policy"));
        if (json_bool(management_status(root), "running"))
            result = send_control(options, command, candidate);
        else {
            if (!durable_idle(root))
                throw Error("NATIVE_DURABLE_WORK_ACTIVE");
            config["previous"] = config.at("current");
            config["current"] = candidate;
            config_write(root, config);
            result = Json{{"staged", true}, {"started", false}, {"sha256", candidate.at("sha256")}};
        }
    } else
        throw Error("Unknown native management command " + command);
    std::cout << result.dump(2) << '\n';
    return 0;
}
} // namespace devbox
