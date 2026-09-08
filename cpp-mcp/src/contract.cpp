#include "devbox/contract.hpp"
#include "reference_contract.hpp"
#include <algorithm>
#include <cmath>
#include <re2/re2.h>
namespace devbox {
namespace {
const std::set<std::string> agent_names{
    "devbox_capabilities", "devbox_file_state", "devbox_write_file_atomic", "devbox_job_submit",
    "devbox_job_list",     "devbox_task_get",   "devbox_task_put",          "devbox_task_list"};
std::string render(std::string text, const Config& config, const std::string& name, bool description) {
    text = replace_all(text, "__DEVBOX_WORKSPACE__", path_text(config.devbox_workspace_path));
    text = replace_all(text, "__HOST_WORKDIR__", path_text(config.host_default_workdir));
    text = replace_all(text, "__DEVBOX_USER__", config.devbox_default_user);
    if (agent_names.contains(name))
        text = replace_all(text, "Rust", "C++");
    if (config.runtime_mode == RuntimeMode::host) {
        text = replace_all(text, "Windows Host Devbox", config.platform.display_name + " Host Devbox");
        text = replace_all(text, "Windows host devbox", config.platform.display_name + " host devbox");
    }
    const std::set<std::string> dynamic{"host_status",
                                        "windows_host_status",
                                        "host_capture_display",
                                        "host_capture_window",
                                        "host_capture_program",
                                        "windows_host_capture_display",
                                        "windows_host_capture_program",
                                        "host_exec",
                                        "windows_host_exec",
                                        "host_run_program",
                                        "windows_host_run_program"};
    if (!config.platform.is_windows && dynamic.contains(name)) {
        if (name == "host_exec" && description)
            return "Use this when you explicitly need native " + lower(config.platform.display_name) +
                   " host tooling rather than the " + config.runtime_label() +
                   ", such as shell automation, git, node, python, or other host commands.";
        text = replace_all(text, "Windows PowerShell", config.platform.display_name + " Host Shell");
        text = replace_all(text, "Windows Host", config.platform.display_name + " Host");
        text = replace_all(text, "windows host", lower(config.platform.display_name) + " host");
    }
    return text;
}
bool type_matches(const Json& value, std::string_view type) {
    if (type == "null")
        return value.is_null();
    if (type == "string")
        return value.is_string();
    if (type == "integer")
        return value.is_number_integer();
    if (type == "number")
        return value.is_number();
    if (type == "boolean")
        return value.is_boolean();
    if (type == "array")
        return value.is_array();
    if (type == "object")
        return value.is_object();
    return true;
}
std::string parameter_type(const Json& schema) {
    const auto types = schema.value("type", Json("object"));
    auto type = types.is_string() ? types.get<std::string>() : std::string("object");
    if (types.is_array())
        for (const auto& item : types)
            if (item != "null") {
                type = item.get<std::string>();
                break;
            }
    if (type == "number")
        return "f64";
    if (type == "integer") {
        const auto format = json_string(schema, "format");
        return format == "uint32" ? "u32" : format == "int64" ? "i64" : "u64";
    }
    if (type == "boolean")
        return "a boolean";
    if (type == "string")
        return "a string";
    if (type == "array")
        return "a sequence";
    return "a map";
}
std::string unexpected_value(const Json& value) {
    if (value.is_string())
        return "string " + value.dump();
    if (value.is_boolean())
        return "boolean `" + value.dump() + "`";
    if (value.is_number())
        return std::string(value.is_number_integer() ? "integer `" : "floating point `") + value.dump() + "`";
    if (value.is_array())
        return "sequence";
    if (value.is_null())
        return "null";
    return "map";
}
void validate(const Json& value, const Json& schema, const Json& root, const std::string& path,
              unsigned depth = 0) {
    if (depth > 64)
        throw Error(path + ": schema nesting limit exceeded");
    if (schema.is_boolean()) {
        if (!schema.get<bool>())
            throw Error(path + ": value is not permitted");
        return;
    }
    if (!schema.is_object())
        return;
    if (schema.contains("$ref")) {
        auto ref = json_string(schema, "$ref");
        if (!ref.starts_with("#/"))
            throw Error("Unsupported tool schema reference");
        validate(value, root.at(Json::json_pointer(ref.substr(1))), root, path, depth + 1);
        return;
    }
    for (const auto* choice : {"anyOf", "oneOf"})
        if (schema.contains(choice)) {
            std::size_t accepted = 0;
            for (const auto& branch : schema[choice])
                try {
                    validate(value, branch, root, path, depth + 1);
                    ++accepted;
                } catch (const Error&) {
                }
            if (!accepted || (std::string_view(choice) == "oneOf" && accepted != 1))
                throw Error(path + ": value does not match allowed alternatives");
        }
    if (schema.contains("type")) {
        const auto& types = schema["type"];
        bool allowed = false;
        if (types.is_string())
            allowed = type_matches(value, types.get<std::string>());
        else
            for (const auto& type : types)
                allowed = allowed || type_matches(value, type.get<std::string>());
        if (!allowed)
            throw ParameterError("failed to deserialize parameters: invalid type: " +
                                 unexpected_value(value) + ", expected " + parameter_type(schema));
    }
    if (schema.contains("enum") &&
        std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end())
        throw Error(path + ": value is not in the allowed enum");
    if (value.is_number()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number) ||
            (schema.contains("minimum") && number < schema["minimum"].get<double>()) ||
            (schema.contains("maximum") && number > schema["maximum"].get<double>()))
            throw Error(path + ": number is outside the allowed range");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::size_t length = 0;
        for (unsigned char ch : text)
            if ((ch & 0xc0) != 0x80)
                ++length;
        if ((schema.contains("minLength") && length < json_uint(schema, "minLength")) ||
            (schema.contains("maxLength") && length > json_uint(schema, "maxLength")))
            throw Error(path + ": string length is outside the allowed range");
        if (schema.contains("pattern") && !RE2::PartialMatch(text, RE2(json_string(schema, "pattern"))))
            throw Error(path + ": string does not match the required pattern");
    }
    if (value.is_array()) {
        if ((schema.contains("minItems") && value.size() < json_uint(schema, "minItems")) ||
            (schema.contains("maxItems") && value.size() > json_uint(schema, "maxItems")))
            throw Error(path + ": array length is outside the allowed range");
        if (schema.contains("items"))
            for (const auto& item : value)
                validate(item, schema["items"], root, path + "[]", depth + 1);
    }
    if (value.is_object()) {
        for (const auto& key : schema.value("required", Json::array()))
            if (!value.contains(key.get<std::string>()))
                throw ParameterError("failed to deserialize parameters: missing field `" +
                                     key.get<std::string>() + "`");
        const auto properties = schema.value("properties", Json::object());
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (properties.contains(it.key()))
                validate(it.value(), properties[it.key()], root, path + "." + it.key(), depth + 1);
            else if (schema.contains("additionalProperties"))
                validate(it.value(), schema["additionalProperties"], root, path + "." + it.key(), depth + 1);
        }
    }
}
} // namespace
ToolContract::ToolContract(const Config& config) {
    static const auto reference = Json::parse(
        std::string_view(reinterpret_cast<const char*>(embedded_contract), sizeof(embedded_contract)));
    tools_ = reference["profiles"][config.runtime_name()];
    for (auto& tool : tools_) {
        const auto name = json_string(tool, "name");
        for (const auto* field : {"description", "title"})
            if (tool.contains(field))
                tool[field] = render(tool[field].get<std::string>(), config, name,
                                     std::string_view(field) == "description");
        auto& properties = tool["inputSchema"]["properties"];
        auto set = [&](const char* key, const char* field, Json value) {
            if (properties.contains(key))
                properties[key][field] = std::move(value);
        };
        if (agent_names.contains(name))
            set("content_base64", "maxLength",
                std::min<std::uint64_t>(config.max_mcp_transfer_chars, max_safe_integer));
        else {
            set("working_dir", "default",
                path_text(name.starts_with("host_") || name.starts_with("windows_host_")
                              ? config.host_default_workdir
                              : config.devbox_workspace_path));
            set("user", "default", config.devbox_default_user);
            if (name == "devbox_list_files" || name == "devbox_search_files")
                set("path", "default", path_text(config.devbox_workspace_path));
            set("max_output_chars", "default", config.command_output_limit_chars);
            set("max_output_chars", "maximum", config.command_output_limit_chars);
            set("max_bytes", "maximum",
                std::min<std::uint64_t>(config.max_mcp_transfer_chars, max_safe_integer));
            set("content_max_bytes", "maximum",
                std::min<std::uint64_t>(config.max_mcp_transfer_chars, max_safe_integer));
            const double wait = std::min(85.0, config.max_wait_seconds);
            set("seconds", "maximum", wait);
            set("wait_seconds", "maximum", wait);
            if (name == "devbox_wait_for_file") {
                set("timeout_seconds", "maximum", wait);
                set("timeout_seconds", "default", std::min(60.0, wait));
            }
        }
    }
}
Json ToolContract::selected(const std::set<std::string>& implemented) const {
    Json result = Json::array();
    for (const auto& tool : tools_)
        if (implemented.contains(json_string(tool, "name")))
            result.push_back(tool);
    return result;
}
Json ToolContract::tool(std::string_view name) const {
    for (const auto& tool : tools_)
        if (json_string(tool, "name") == name)
            return tool;
    throw Error("Unknown tool");
}
Json ToolContract::arguments(std::string_view name, const Json& supplied) const {
    const auto schema = tool(name)["inputSchema"];
    validate(supplied, schema, schema, "arguments");
    auto result = supplied;
    const auto properties = schema.value("properties", Json::object());
    for (auto it = properties.begin(); it != properties.end(); ++it)
        if (!result.contains(it.key()) && it.value().is_object() && it.value().contains("default"))
            result[it.key()] = it.value()["default"];
    return result;
}
Json ToolContract::capabilities(const Config& config, const std::set<std::string>& implemented) const {
    auto tools = selected(implemented);
    std::sort(tools.begin(), tools.end(),
              [](const Json& a, const Json& b) { return json_string(a, "name") < json_string(b, "name"); });
    Json names = Json::array();
    for (const auto& tool : tools)
        names.push_back(tool["name"]);
    return Json{{"contract_version", 2},
                {"implementation", "cpp"},
                {"schema_sha256", sha256(tools.dump())},
                {"tools", names},
                {"resource_classes", {"auto", "watch", "light", "heavy", "io-heavy"}},
                {"limits",
                 {{"execution", config.exec_max_concurrent},
                  {"reserved_interactive", config.exec_reserved_interactive},
                  {"heavy_capacity", config.exec_heavy_capacity},
                  {"watch_capacity", config.watch_max_concurrent},
                  {"active_runners", config.job_max_active},
                  {"runners_per_task", config.job_max_per_task},
                  {"operation_receipts", config.job_max_operations},
                  {"task_state_bytes", 65536}}},
                {"build", build_snapshot()}};
}
} // namespace devbox
