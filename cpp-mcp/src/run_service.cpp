#include "devbox/run_service.hpp"
#include "devbox/contract.hpp"
#include <algorithm>
#include <thread>
namespace devbox {
namespace {
Json model_tools() {
    return Json::array(
        {Json{{"type", "function"},
              {"function",
               {{"name", "program"},
                {"description",
                 "Propose one program execution inside this run's isolated workspace. An operator must grant "
                 "the exact executable identity and arguments before execution. Network and child processes "
                 "are denied; do not claim success until a tool result confirms it."},
                {"parameters",
                 {{"type", "object"},
                  {"properties",
                   {{"requested_program",
                     {{"type", "string"},
                      {"description",
                       "Program requested for operator review; the grant selects the actual executable."}}},
                    {"args", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 256}}},
                    {"input", {{"type", "string"}, {"maxLength", 65536}}},
                    {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", 30000}}},
                    {"output_chars", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}}}},
                  {"required", {"requested_program", "args"}},
                  {"additionalProperties", false}}}}}}});
}
ProviderProfile decode_profile(const Json& value) {
    ProviderProfile profile;
    profile.id = json_string(value, "id");
    validate_key(profile.id);
    profile.base_url = json_string(value, "base_url");
    while (profile.base_url.ends_with('/'))
        profile.base_url.pop_back();
    profile.model = json_string(value, "model");
    const auto protocol = json_string(value, "protocol");
    if (protocol != "responses" && protocol != "chat_completions")
        throw Error("RUN_PROVIDER_PROTOCOL_INVALID");
    profile.protocol =
        protocol == "responses" ? ProviderProtocol::Responses : ProviderProtocol::ChatCompletions;
    profile.local = json_bool(value, "local");
    profile.tools = json_bool(value, "tools");
    profile.vision = json_bool(value, "vision");
    profile.streaming = json_bool(value, "streaming", true);
    profile.background_cancel = json_bool(value, "background_cancel");
    profile.context_tokens = json_uint(value, "context_tokens", 32768);
    profile.output_tokens = json_uint(value, "output_tokens", 4096);
    profile.required_vram_bytes = json_uint(value, "required_vram_bytes");
    if (value.contains("input_micro_usd_per_million"))
        profile.input_micro_usd_per_million = json_uint(value, "input_micro_usd_per_million");
    if (value.contains("output_micro_usd_per_million"))
        profile.output_micro_usd_per_million = json_uint(value, "output_micro_usd_per_million");
    if (value.contains("api_key") || value.contains("secret") || value.contains("authorization"))
        throw Error("RUN_PROVIDER_LITERAL_CREDENTIAL_DENIED: configure a private secret_file reference");
    return profile;
}
Json worker_record(std::string_view principal, std::string_view id, std::uint32_t pid,
                   std::optional<std::uint64_t> instance) {
    return Json{{"principal", principal},
                {"run_id", id},
                {"pid", pid},
                {"instance", instance ? Json(std::to_string(*instance)) : Json()},
                {"status", "running"}};
}
} // namespace
std::shared_ptr<StateStore> RunService::state() const {
    if (!jobs_.indexed())
        throw Error("RUN_REQUIRES_INDEXED_STATE: migrate and select SQLite first");
    return jobs_.index();
}
Json RunService::profiles() const {
    if (config_->state_backend != "sqlite")
        return Json::array();
    const auto path = config_->state_root / "providers.json";
    const auto config = read_json_optional(path, 131072);
    if (!config)
        return Json::array();
    const auto rows = config->at("profiles");
    if (!rows.is_array() || rows.size() > 16)
        throw Error("RUN_PROVIDER_CONFIGURATION_LIMIT");
    Json result = Json::array();
    for (const auto& row : rows) {
        const auto value = decode_profile(row);
        result.push_back(Json{
            {"id", value.id},
            {"model", value.model},
            {"local", value.local},
            {"tools", value.tools},
            {"protocol", value.protocol == ProviderProtocol::Responses ? "responses" : "chat_completions"},
            {"max_run_cost_micro_usd", json_uint(row, "max_run_cost_micro_usd")},
            {"capability_source", "operator_configured"}});
    }
    return result;
}
Json RunService::profile(std::string_view id) const {
    validate_key(id);
    ensure_private_state_directory(config_->state_root);
    const auto path = config_->state_root / "providers.json";
    if (fs::is_symlink(fs::symlink_status(path)))
        throw Error("RUN_PROVIDER_CONFIG_ALIAS_DENIED");
    const auto config = read_json_optional(path, 131072);
    if (!config)
        throw Error("RUN_PROVIDER_NOT_CONFIGURED");
    if (!config->contains("profiles") || !(*config)["profiles"].is_array() ||
        (*config)["profiles"].size() > 16)
        throw Error("RUN_PROVIDER_CONFIGURATION_LIMIT");
    std::optional<Json> found;
    for (const auto& row : (*config)["profiles"])
        if (json_string(row, "id") == id) {
            if (found)
                throw Error("RUN_PROVIDER_DUPLICATE_ID");
            found = row;
        }
    if (!found)
        throw Error("RUN_PROVIDER_NOT_CONFIGURED");
    (void)decode_profile(*found);
    return *found;
}
Json RunService::call(std::string_view principal, const Json& args) {
    validate_key(principal);
    const auto action = json_string(args, "action");
    if (action == "providers")
        return Json{{"providers", profiles()}, {"isolation", isolation_capabilities()}};
    auto index = state();
    GrantAuthority grants(index, config_->state_root / "grants");
    RunController controller(index, config_->state_root / "runs", grants);
    const auto id = json_string(args, "run_id");
    if (action == "create") {
        if (config_->runtime_mode != RuntimeMode::host || !config_->host_exec_enabled)
            throw Error("RUN_REQUIRES_ENABLED_HOST_RUNTIME");
        const auto configured = profile(json_string(args, "provider_id"));
        const auto provider = decode_profile(configured);
        RunSpec spec;
        spec.principal = principal;
        spec.request_id = json_string(args, "request_id");
        spec.goal = json_string(args, "goal");
        spec.provider_id = provider.id;
        spec.provider_fingerprint = sha256(canonical_json(configured).dump());
        if (provider.tools)
            spec.tools = model_tools();
        spec.tool_schema_sha256 = sha256(canonical_json(spec.tools).dump());
        spec.budget.rounds = json_uint(args, "max_rounds", 16);
        spec.budget.tool_calls = json_uint(args, "max_tool_calls", 32);
        spec.budget.tokens = json_uint(args, "max_tokens", 200000);
        spec.budget.cost_micro_usd = json_uint(args, "max_cost_micro_usd", 0);
        spec.budget.wall_ms = json_uint(args, "timeout_seconds", 300) * 1000;
        spec.budget.call_tokens = std::min(provider.context_tokens, spec.budget.tokens);
        spec.budget.output_tokens = std::min<std::uint64_t>(
            provider.output_tokens, std::max<std::uint64_t>(1, spec.budget.call_tokens / 4));
        spec.budget.call_cost_micro_usd =
            provider_cost_ceiling(provider, provider.context_tokens, spec.budget.output_tokens);
        if (spec.budget.cost_micro_usd > json_uint(configured, "max_run_cost_micro_usd", 0) ||
            spec.budget.call_cost_micro_usd > spec.budget.cost_micro_usd)
            throw Error("RUN_OPERATOR_COST_POLICY");
        const auto created = controller.create(spec);
        const auto run_id = json_string(created, "run_id");
        ensure_private_state_directory(config_->state_root / "isolated");
        ensure_private_state_directory(config_->state_root / "isolated" / run_id);
        auto result = created;
        if (!json_bool(created, "terminal"))
            result["driver"] = start_driver(principal, run_id);
        return result;
    }
    if (action == "get") {
        auto result = controller.get(principal, id);
        result["workspace"] = path_text(config_->state_root / "isolated" / std::string(id));
        return result;
    }
    if (action == "events")
        return Json{{"events", controller.events(principal, id, json_uint(args, "after"),
                                                 json_uint(args, "limit", 50))}};
    if (action == "artifact")
        return Json{{"untrusted_content", true},
                    {"data", controller.read_artifact(principal, id, json_string(args, "sha256"))}};
    Json result;
    if (action == "approve")
        result = controller.approve(principal, id, json_string(args, "grant_id"));
    else if (action == "reconcile")
        result = controller.reconcile(principal, id, json_string(args, "resolution"),
                                      args.value("observed_result", Json::object()));
    else
        result = controller.control(principal, id, action);
    if (action == "resume" || action == "approve" || (action == "reconcile" && result["status"] == "ready"))
        result["driver"] = start_driver(principal, id);
    return result;
}
Json RunService::start_driver(std::string_view principal, std::string_view id) {
    auto index = state();
    const auto run = index->get("run", id);
    if (!run || run->principal != principal)
        throw Error("RUN_NOT_FOUND");
    const auto directory = config_->state_root / "runs" / run->id;
    FileLock launch(directory / ".launch.lock", Millis(5000), {}, true);
    if (const auto owner = read_json_optional(directory / "driver-owner.json", 4096)) {
        const auto pid = json_uint(*owner, "pid");
        const auto instance =
            owner_process_instance(Json{{"processInstance", owner->value("instance", Json())}});
        if (json_string(*owner, "status") != "retiring" && pid && pid <= UINT32_MAX && instance &&
            process_matches_instance(static_cast<std::uint32_t>(pid), instance))
            return Json{{"pid", pid}, {"replayed", true}};
    }
    std::optional<std::uint64_t> instance;
    const auto pid =
        spawn_detached(executable_path(), {"--agent-runner", std::string(principal), std::string(id)},
                       config_->project_root, background_environment(*config_), &instance);
    write_json_atomic(directory / "driver-owner.json", worker_record(principal, id, pid, instance));
    return Json{{"pid", pid}, {"replayed", false}};
}
void RunService::recover(const Cancel& cancel) {
    if (config_->state_backend != "sqlite" || config_->runtime_mode != RuntimeMode::host ||
        !config_->host_exec_enabled)
        return;
    auto index = state();
    ensure_private_state_directory(config_->state_root / "runs");
    FileLock recovery(config_->state_root / "runs" / ".recovery.lock", Millis(1000), cancel, true);
    for (const auto* status : {"ready", "running"}) {
        std::optional<std::string> cursor;
        std::size_t inspected = 0;
        do {
            if (cancel)
                cancel->check();
            const auto page = index->list(StateQuery{"run", {}, {}, std::string(status), cursor, 100});
            for (const auto& run : page.records) {
                if (++inspected > 4096)
                    throw Error("RUN_RECOVERY_SCAN_BUDGET");
                if (cancel)
                    cancel->check();
                (void)start_driver(run.principal, run.id);
            }
            cursor = page.next;
        } while (cursor);
    }
}
int RunService::drive_impl(std::string_view principal, std::string_view id,
                           const std::function<bool()>& stop) {
    auto index = state();
    GrantAuthority grants(index, config_->state_root / "grants");
    RunController controller(index, config_->state_root / "runs", grants);
    (void)controller.get(principal, id);
    const auto directory = config_->state_root / "runs" / std::string(id);
    {
        FileLock launch(directory / ".launch.lock", Millis(5000), {}, true);
        write_json_atomic(directory / "driver-owner.json",
                          worker_record(principal, id, process_id(), process_instance(process_id())));
    }
    const auto record = index->get("run", id);
    const auto configured = profile(json_string(record->data, "provider_id"));
    if (sha256(canonical_json(configured).dump()) != json_string(record->data, "provider_fingerprint"))
        throw Error("RUN_PROVIDER_CONFIGURATION_CHANGED");
    auto provider_profile = decode_profile(configured);
    const auto expected_tools = provider_profile.tools ? model_tools() : Json::array();
    if (sha256(canonical_json(expected_tools).dump()) != json_string(record->data, "tool_schema_sha256"))
        throw Error("RUN_TOOL_SCHEMA_CHANGED");
    const SecretScope scope{std::string(principal), std::string(id), provider_profile.base_url};
    SecretBroker secrets;
    if (const auto file = json_string(configured, "secret_file"); !file.empty())
        provider_profile.secret_reference =
            secrets.bind(path_from_utf8(file), scope, json_uint(record->data, "expires_at_ms"));
    ModelProvider provider(provider_profile, &secrets);
    auto cancellation = std::make_shared<Cancellation>();
    std::atomic_bool finished{false};
    std::thread monitor([&] {
        while (!finished) {
            try {
                const auto current = controller.get(principal, id);
                if ((stop && stop()) || json_string(current, "control") == "cancel" ||
                    unix_millis() >= json_uint(current, "expires_at_ms")) {
                    cancellation->cancel();
                    return;
                }
            } catch (...) {
                cancellation->cancel();
                return;
            }
            if (cancellation->wait_for(Millis(100)))
                return;
        }
    });
    ScopeExit join([&] {
        finished = true;
        cancellation->cancel();
        monitor.join();
    });
    auto scheduler_config = SchedulerConfig::from(*config_);
    ExecutionScheduler scheduler(scheduler_config);
    RunHooks hooks;
    hooks.model = [&](const ProviderRequest& supplied) {
        auto request = supplied;
        request.stream = provider_profile.streaming;
        request.budget.available_vram_bytes = json_uint(configured, "available_vram_bytes");
        if (provider_profile.local && provider_profile.required_vram_bytes &&
            !config_->exec_gpu_capacity_bytes)
            throw Error("RUN_GPU_ADMISSION_CAPACITY_REQUIRED: configure MCP_EXEC_GPU_CAPACITY_BYTES for "
                        "local GPU models");
        AcquireRequest admission{ExecutionKind::background, ResourceClass::light, 1, "agent-model", {}};
        if (provider_profile.local)
            admission.resources.gpu_bytes = provider_profile.required_vram_bytes;
        auto lease = scheduler.acquire(admission, cancellation);
        return provider.generate(request, scope, cancellation);
    };
    hooks.tool = [&](const GrantDefinition& definition) {
        if (definition.tool != "program" || !definition.egress_origins.empty())
            throw Error("RUN_TOOL_BROKER_UNAVAILABLE");
        const auto workspace = config_->state_root / "isolated" / std::string(id);
        if (fs::canonical(definition.workspace) != fs::canonical(workspace))
            throw Error("RUN_WORKSPACE_GRANT_MISMATCH");
        IsolatedProgram request;
        request.private_root = config_->state_root / "isolated";
        request.workspace = workspace;
        request.executable = definition.executable;
        request.executable_sha256 = definition.executable_sha256.value_or("");
        request.arguments = json_strings(definition.arguments, "args");
        if (definition.arguments.contains("input"))
            request.input = json_string(definition.arguments, "input");
        request.timeout = Millis(json_uint(definition.arguments, "timeout_ms", 30000));
        request.output_chars = json_uint(definition.arguments, "output_chars", 4096);
        AcquireRequest admission{ExecutionKind::background, ResourceClass::light, 1, "agent-program", {}};
        admission.resources.memory_bytes = request.memory_bytes;
        auto lease = scheduler.acquire(admission, cancellation);
        const auto output = run_isolated_program(request, cancellation);
        return Json{{"exit_code", output.exit_code},
                    {"stdout", output.stdout_text},
                    {"stderr", output.stderr_text},
                    {"stdout_truncated", output.stdout_capture_truncated},
                    {"stderr_truncated", output.stderr_capture_truncated},
                    {"isolation", isolation_capabilities()}};
    };
    for (std::size_t steps = 0; steps < 512; ++steps) {
        const auto result = controller.step(principal, id, hooks, cancellation);
        const auto status = json_string(result, "status");
        if (json_bool(result, "terminal") || status == "awaiting_approval" || status == "paused" ||
            status == "uncertain") {
            FileLock launch(directory / ".launch.lock", Millis(5000), {}, true);
            const auto latest = controller.get(principal, id);
            if (json_string(latest, "status") == "ready" && !json_bool(latest, "terminal"))
                continue;
            auto owner = worker_record(principal, id, process_id(), process_instance(process_id()));
            owner["status"] = "retiring";
            write_json_atomic(directory / "driver-owner.json", owner);
            return 0;
        }
    }
    throw Error("RUN_DRIVER_STEP_BUDGET");
}
int RunService::drive(std::string_view principal, std::string_view id, const std::function<bool()>& stop) {
    try {
        validate_key(principal);
        validate_key(id);
        auto index = state();
        const auto initial = index->get("run", id);
        if (!initial || initial->principal != principal)
            return 1;
        const auto directory = config_->state_root / "runs" / initial->id;
        // Keep ownership through failure publication. A contender that cannot
        // obtain this lock must not change the real driver's state or events.
        FileLock driver(directory / ".driver.lock", Millis(1000), {}, true);
        const auto current = index->get("run", id);
        if (!current || current->principal != principal)
            return 1;
        if (current->status == "completed" || current->status == "failed" || current->status == "cancelled")
            return 0;
        try {
            return drive_impl(principal, id, stop);
        } catch (...) {
            // The controller lease also excludes an in-flight state-machine step.
            // Revision-CAS below still protects concurrent operator control writes.
            FileLock controller(directory / ".controller.lock", Millis(1000), {}, true);
            const auto latest = index->get("run", id);
            if (latest && latest->principal == principal && latest->status != "completed" &&
                latest->status != "failed" && latest->status != "cancelled") {
                auto record = *latest;
                const auto phase = json_string(record.data, "phase");
                record.status = phase.ends_with("_pending") ? "uncertain" : "failed";
                record.data["error_code"] = "RUN_DRIVER_FAILURE";
                record.data["updated_at"] = utc_now();
                StateMutation update{record, record.revision};
                StateEvent event{record.id, "driver_failed", 0, Json{{"status", record.status}}};
                index->apply({&update, 1}, {&event, 1});
            }
            return 1;
        }
    } catch (...) {
        // Startup/ownership failures (or a racing CAS) carry no authority to
        // overwrite another driver, a terminal result, or an operator decision.
        return 1;
    }
}
} // namespace devbox
