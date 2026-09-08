#include "devbox/engine.hpp"
#include "devbox/result.hpp"
namespace devbox {
namespace {
std::optional<std::string> optional_text(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? std::optional(value[key].get<std::string>())
                                                         : std::nullopt;
}
std::string payload(const Json& args) {
    const auto content = optional_text(args, "content"), encoded = optional_text(args, "content_base64");
    if (content && encoded)
        throw Error("Provide either content or content_base64, not both.");
    if (!content && !encoded)
        throw Error("Either content or content_base64 is required.");
    return content ? base64_encode(*content) : *encoded;
}
Json large_result(const std::string& summary, const Json& data, bool read) {
    Json metadata = data;
    if (read) {
        metadata.erase("content_base64");
        metadata["content_base64_chars"] = json_string(data, "content_base64").size();
    } else {
        metadata = Json::object();
        for (const auto* key :
             {"path", "append", "previous_file_size", "final_file_size", "bytes_written", "content_sha256",
              "verification_mode", "verified", "expected_sha256_verified", "target_existed"})
            metadata[key] = data.value(key, Json());
    }
    return result_explicit(summary, Json(data), summary + "\n\n" + canonical_json(metadata).dump(2));
}
} // namespace
Json Engine::files(std::string name, const Json& args, const Cancel& cancel) {
    const bool host = name.starts_with("windows_host_");
    const bool docker = !host && config_->runtime_mode == RuntimeMode::docker;
    if (host && !config_->host_exec_enabled)
        return result_error(name == "windows_host_inspect_file" ? "Host execution is disabled."
                                                                : "Windows host execution is disabled.");
    const auto requested = json_string(args, "path");
    const auto path =
        host ? resolve_windows_host_path(requested, working_dir(args, true)) : path_from_utf8(requested);
    if (name == "windows_host_inspect_file") {
        InspectFileRequest inspect;
        inspect.path = requested;
        inspect.working_dir = working_dir(args, true);
        inspect.resolved_path = path;
        inspect.max_bytes = json_uint(args, "max_bytes", 65536);
        return result_success("Inspected " + requested + " on the Windows host.",
                              inspect_host_file(*config_, runtime_, inspect, cancel));
    }
    if (name == "devbox_read_large_file" || name == "windows_host_read_large_file") {
        try {
            const auto offset = json_uint(args, "offset_bytes"),
                       maximum = json_uint(args, "max_bytes", 262144);
            const auto data = docker ? docker_files_.read_large(requested, offset, maximum, cancel)
                                     : read_large(path, offset, maximum);
            const auto summary =
                "Read " + requested + " from byte " + std::to_string(offset) +
                (host ? " on the Windows host." : " in the " + config_->runtime_label() + ".");
            return large_result(summary, data, true);
        } catch (const std::exception& e) {
            return result_error(host ? e.what() : "Failed to read " + requested + ": " + e.what());
        }
    }
    if (name == "devbox_write_large_file" || name == "windows_host_write_large_file") {
        const auto encoded = payload(args);
        const bool append = json_bool(args, "append");
        try {
            const auto data = docker
                                  ? docker_files_.write_large(requested, encoded, append,
                                                              json_bool(args, "create_dirs", true),
                                                              optional_text(args, "expected_sha256"), cancel)
                                  : write_large(path, encoded, append, json_bool(args, "create_dirs", true),
                                                optional_text(args, "expected_sha256"));
            const auto summary =
                std::string(append ? "Appended large payload to " : "Wrote large payload to ") + requested +
                (host ? " on the Windows host" : " in the " + config_->runtime_label()) +
                " and verified the exact bytes.";
            return large_result(summary, data, false);
        } catch (const std::exception& e) {
            return result_error(host ? e.what()
                                     : "Failed to write large payload to " + requested + ": " + e.what());
        }
    }
    try {
        ProcessOutput output;
        std::string summary;
        if (name == "devbox_list_files") {
            const auto directory =
                trim(requested).empty() ? path_text(config_->devbox_workspace_path) : requested;
            ListOptions list;
            list.path = path_from_utf8(directory);
            list.recursive = json_bool(args, "recursive");
            list.max_depth = json_uint(args, "max_depth", 4);
            list.max_entries = json_uint(args, "max_entries", 5000);
            list.timeout = Millis(json_uint(args, "timeout_seconds", 30) * 1000);
            list.exclude_directories = json_strings(args, "exclude_directories");
            output = docker ? docker_files_.list(directory, list.recursive, list.max_depth, list.max_entries,
                                                 list.timeout, list.exclude_directories, cancel)
                            : list_files(list, cancel);
            summary = "Listed files in " + directory + ".";
        } else if (name == "devbox_read_file") {
            if (docker)
                output = docker_files_.read_text(requested, json_uint(args, "max_bytes", 65536), cancel);
            else
                output.stdout_text = read_text(path, json_uint(args, "max_bytes", 65536));
            summary = "Read " + requested + " from the " + config_->runtime_label() + ".";
        } else if (name == "devbox_write_file") {
            if (docker)
                output = docker_files_.write_text(requested, json_string(args, "content"),
                                                  json_bool(args, "append"),
                                                  json_bool(args, "create_dirs", true), cancel);
            else
                atomic_write(path, json_string(args, "content"), json_bool(args, "append"),
                             json_bool(args, "create_dirs", true));
            summary = std::string(json_bool(args, "append") ? "Appended text to " : "Wrote ") + requested +
                      " in the " + config_->runtime_label() + ".";
        } else
            throw Error("Unknown file tool: " + name);
        return render_file_output(summary, output, config_->command_output_limit_chars);
    } catch (const std::exception& e) {
        return result_process(e.what(), std::nullopt, "", "", std::nullopt, false);
    }
}
} // namespace devbox
