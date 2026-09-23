#include "devbox/grants.hpp"
#include <algorithm>
#include <array>
namespace devbox {
namespace {
void context_valid(const GrantContext& context) {
    validate_key(context.principal);
    validate_key(context.run);
    validate_key(context.operation);
}
std::string operation_key(const GrantContext& context) {
    return sha256(Json::array({context.principal, context.run, context.operation}).dump());
}
std::string arguments_hash(const Json& arguments) {
    if (!arguments.is_object() || arguments.dump().size() > 65536)
        throw Error("GRANT_ARGUMENT_BUDGET");
    return sha256(canonical_json(arguments).dump());
}
void no_alias_path(const fs::path& path, bool directory) {
    if (!path.is_absolute())
        throw Error("GRANT_ABSOLUTE_PATH_REQUIRED");
    auto cursor = path.root_path();
    for (const auto& component : path.relative_path()) {
        if (component == ".." || component == ".")
            throw Error("GRANT_PATH_ALIAS_DENIED");
        cursor /= component;
        const auto status = fs::symlink_status(cursor);
        if (fs::is_symlink(status))
            throw Error("GRANT_PATH_ALIAS_DENIED");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error("GRANT_PATH_ALIAS_DENIED");
#endif
    }
    if (directory ? !fs::is_directory(path) : !fs::is_regular_file(path))
        throw Error("GRANT_PATH_TYPE_MISMATCH");
    if (!directory && fs::hard_link_count(path) != 1)
        throw Error("GRANT_EXECUTABLE_ALIAS_DENIED");
}
Json encode(const GrantDefinition& definition) {
    return Json{
        {"principal", definition.context.principal},
        {"run", definition.context.run},
        {"operation", definition.context.operation},
        {"tool", definition.tool},
        {"arguments", definition.arguments},
        {"arguments_sha256", arguments_hash(definition.arguments)},
        {"workspace", path_text(definition.workspace)},
        {"executable", path_text(definition.executable)},
        {"executable_sha256", definition.executable_sha256 ? Json(*definition.executable_sha256) : Json()},
        {"egress_origins", definition.egress_origins},
        {"expires_at_ms", definition.expires_at_ms}};
}
GrantDefinition decode(const Json& value) {
    GrantDefinition result;
    result.context = {json_string(value, "principal"), json_string(value, "run"),
                      json_string(value, "operation")};
    result.tool = json_string(value, "tool");
    result.arguments = value.at("arguments");
    result.workspace = path_from_utf8(json_string(value, "workspace"));
    result.executable = path_from_utf8(json_string(value, "executable"));
    if (value.at("executable_sha256").is_string())
        result.executable_sha256 = value["executable_sha256"].get<std::string>();
    result.egress_origins = json_strings(value, "egress_origins");
    result.expires_at_ms = json_uint(value, "expires_at_ms");
    return result;
}
void paths_valid(const GrantDefinition& definition) {
    if (!definition.workspace.empty())
        no_alias_path(definition.workspace, true);
    if (!definition.executable.empty()) {
        no_alias_path(definition.executable, false);
        const auto extension = lower(path_text(definition.executable.extension()));
        if (extension == ".cmd" || extension == ".bat" || extension == ".ps1" || extension == ".sh")
            throw Error("GRANT_DIRECT_EXECUTABLE_REQUIRED");
        if (fs::file_size(definition.executable) > 256 * 1024 * 1024)
            throw Error("GRANT_EXECUTABLE_BUDGET");
        if (!definition.executable_sha256 ||
            *definition.executable_sha256 != sha256_file(definition.executable))
            throw Error("GRANT_EXECUTABLE_IDENTITY_CHANGED");
    } else if (definition.executable_sha256)
        throw Error("GRANT_EXECUTABLE_REQUIRED");
}
} // namespace
GrantAuthority::GrantAuthority(std::shared_ptr<StateStore> state, fs::path root, GrantOptions options)
    : state_(std::move(state)), root_(std::move(root)), options_(std::move(options)),
      audit_(root_ / "security-audit.jsonl", options_.audit_max_bytes) {
    if (!state_)
        throw Error("GRANT_REQUIRES_INDEXED_STATE");
    ensure_directory(root_.parent_path());
    ensure_private_state_directory(root_);
}
std::string GrantAuthority::issue(const GrantDefinition& definition) {
    context_valid(definition.context);
    validate_key(definition.tool);
    if (definition.expires_at_ms <= options_.now())
        throw Error("GRANT_EXPIRED");
    if (definition.expires_at_ms - options_.now() > 24ULL * 60 * 60 * 1000)
        throw Error("GRANT_EXPIRY_LIMIT: issue grants for at most 24 hours");
    if (definition.tool == "program" &&
        (definition.workspace.empty() || definition.executable.empty() || !definition.egress_origins.empty()))
        throw Error("GRANT_PROGRAM_REQUIRES_WORKSPACE_EXECUTABLE_AND_NO_EGRESS");
    paths_valid(definition);
    if (definition.egress_origins.size() > 16)
        throw Error("GRANT_EGRESS_BUDGET");
    for (const auto& origin : definition.egress_origins) {
        const auto url = Url::parse(origin);
        if ((url.scheme != "https" && url.scheme != "http") || url.host.empty() ||
            (url.path != "/" && !url.path.empty()) || url.has_query || url.has_fragment ||
            !url.userinfo.empty())
            throw Error("GRANT_EXACT_ORIGIN_REQUIRED");
    }
    const auto id = "grant-" + uuid();
    const auto data = encode(definition);
    FileLock gate(root_ / ".admission.lock", Millis(5000), {}, true);
    audit_.append({AuditDecision::Granted, definition.context.principal, definition.context.run,
                   definition.context.operation, id});
    const StateMutation value{
        StateRecord{"grant", id, definition.context.principal, definition.context.run, "active", 0, data}, 0};
    state_->apply({&value, 1});
    return id;
}
void GrantAuthority::revoke(std::string_view id) {
    validate_key(id);
    FileLock gate(root_ / ".admission.lock", Millis(5000), {}, true);
    auto record = state_->get("grant", id);
    if (!record)
        throw Error("GRANT_NOT_FOUND");
    if (record->status == "revoked")
        return;
    const auto definition = decode(record->data);
    audit_.append({AuditDecision::Revoked, definition.context.principal, definition.context.run,
                   definition.context.operation, std::string(id)});
    record->status = "revoked";
    const StateMutation update{*record, record->revision};
    state_->apply({&update, 1});
}
Json GrantAuthority::inspect(std::string_view id) const {
    validate_key(id);
    const auto record = state_->get("grant", id);
    if (!record)
        throw Error("GRANT_NOT_FOUND");
    // Operational/export view deliberately excludes exact arguments, executable paths and hashes.
    return Json{{"grant_id", id},
                {"principal", record->principal},
                {"run", record->group},
                {"operation", record->data.at("operation")},
                {"tool", record->data.at("tool")},
                {"status", record->status},
                {"expires_at_ms", record->data.at("expires_at_ms")}};
}
Json GrantAuthority::perform(std::string_view id, const GrantContext& context, std::string_view tool,
                             const Json& arguments,
                             const std::function<Json(const GrantDefinition&)>& effect) {
    validate_key(id);
    context_valid(context);
    validate_key(tool);
    const auto operation_id = operation_key(context);
    GrantDefinition definition;
    {
        FileLock gate(root_ / ".admission.lock", Millis(5000), {}, true);
        const auto record = state_->get("grant", id);
        const auto denied = [&] {
            audit_.append(
                {AuditDecision::Denied, context.principal, context.run, context.operation, std::string(id)});
            throw Error("GRANT_POLICY_DENIED");
        };
        if (!record || record->principal != context.principal || record->group != context.run ||
            json_string(record->data, "operation") != context.operation ||
            json_string(record->data, "tool") != tool ||
            json_string(record->data, "arguments_sha256") != arguments_hash(arguments))
            denied();
        if (const auto prior = state_->get("grant_operation", operation_id)) {
            if (json_string(prior->data, "grant") != id)
                throw Error("GRANT_OPERATION_CONFLICT");
            if (prior->status != "completed")
                throw Error(
                    "GRANT_OPERATION_UNCERTAIN: reconcile the original operation; do not retry its effect");
            return Json{{"replayed", true}, {"result", prior->data.at("result")}};
        }
        if (record->status != "active" || json_uint(record->data, "expires_at_ms") <= options_.now())
            denied();
        definition = decode(record->data);
        paths_valid(definition);
        audit_.append(
            {AuditDecision::Admitted, context.principal, context.run, context.operation, std::string(id)});
        const StateMutation admission{StateRecord{"grant_operation", operation_id, context.principal,
                                                  context.run, "admitted", 0, Json{{"grant", id}}},
                                      0};
        state_->apply({&admission, 1});
    }
    Json result;
    try {
        result = effect(definition);
        if (result.dump().size() > 65536)
            throw Error("GRANT_RESULT_BUDGET");
    } catch (...) {
        // No retry permission follows a missing acknowledgement. Persist uncertainty even if the
        // failure occurred before the effect; only the responsible broker can reconcile that fact.
        audit_.append(
            {AuditDecision::Uncertain, context.principal, context.run, context.operation, std::string(id)});
        const StateMutation uncertain{StateRecord{"grant_operation", operation_id, context.principal,
                                                  context.run, "uncertain", 0, Json{{"grant", id}}},
                                      1};
        state_->apply({&uncertain, 1});
        throw;
    }
    audit_.append(
        {AuditDecision::Completed, context.principal, context.run, context.operation, std::string(id)});
    const StateMutation completed{StateRecord{"grant_operation", operation_id, context.principal, context.run,
                                              "completed", 0, Json{{"grant", id}, {"result", result}}},
                                  1};
    state_->apply({&completed, 1});
    return Json{{"replayed", false}, {"result", result}};
}
GrantDefinition grant_definition(const Json& value) {
    if (!value.is_object())
        throw Error("GRANT_DEFINITION_OBJECT_REQUIRED");
    return decode(value);
}
Json execute_granted_program(GrantAuthority& authority, const fs::path& private_root, std::string_view id,
                             const GrantContext& context, const Json& arguments, const Cancel& cancel) {
    return authority.perform(id, context, "program", arguments, [&](const GrantDefinition& admitted) {
        IsolatedProgram request;
        request.private_root = private_root;
        request.workspace = admitted.workspace;
        request.executable = admitted.executable;
        request.executable_sha256 = admitted.executable_sha256.value_or("");
        request.arguments = json_strings(arguments, "args");
        if (arguments.contains("input") && !arguments["input"].is_null())
            request.input = json_string(arguments, "input");
        request.timeout = Millis(json_uint(arguments, "timeout_ms", 30000));
        request.memory_bytes = json_uint(arguments, "memory_bytes", 512ULL * 1024 * 1024);
        request.cpu_ms = json_uint(arguments, "cpu_ms", 30000);
        request.output_chars = json_uint(arguments, "output_chars", 4096);
        try {
            const auto output = run_isolated_program(request, cancel);
            return Json{{"status", "succeeded"},
                        {"exit_code", output.exit_code},
                        {"pid", output.pid},
                        {"stdout", output.stdout_text},
                        {"stdout_truncated", output.stdout_capture_truncated},
                        {"stderr_truncated", output.stderr_capture_truncated},
                        {"stderr", output.stderr_text},
                        {"elapsed_ms", output.elapsed_ms},
                        {"isolation", isolation_capabilities()}};
        } catch (const ProcessError& error) {
            // A known process termination is an observed outcome, not permission to rerun its effects.
            if (!error.exit_code || error.timed_out || error.aborted)
                throw;
            return Json{{"status", "application_exit"},   {"exit_code", *error.exit_code},
                        {"stdout", error.stdout_text},    {"stderr", error.stderr_text},
                        {"elapsed_ms", error.elapsed_ms}, {"isolation", isolation_capabilities()}};
        }
    });
}
} // namespace devbox
