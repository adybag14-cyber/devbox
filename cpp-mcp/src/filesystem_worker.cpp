#include "devbox/filesystem_worker.hpp"
#include "devbox/artifacts.hpp"
#include "devbox/jobs.hpp"
#include "devbox/resource_budget.hpp"
#include "devbox/search.hpp"
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <algorithm>
#include <array>
#include <iostream>
namespace devbox {
namespace {
Json path_state(const fs::path& path) {
    Json result;
    std::uint64_t size = 0;
    double millis = 0;
    bool valid_time = false, file = false, directory = false;
#ifdef _WIN32
    NativeHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return Json{{"exists", false}};
        if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION)
            return Json{{"exists", nullptr}, {"transientError", "EPERM"}};
        throw Error(windows_error(code));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info))
        throw Error(windows_error());
    directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    file = !directory;
    size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto ticks =
        (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    if (ticks >= 116444736000000000ULL) {
        millis = static_cast<double>(ticks - 116444736000000000ULL) / 10000.0;
        valid_time = true;
    }
#else
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return Json{{"exists", false}};
        if (errno == EACCES || errno == EPERM)
            return Json{{"exists", nullptr}, {"transientError", "EPERM"}};
        throw Error(std::error_code(errno, std::generic_category()).message());
    }
    directory = S_ISDIR(info.st_mode);
    file = S_ISREG(info.st_mode);
    size = static_cast<std::uint64_t>(info.st_size);
#ifdef __APPLE__
    const auto stamp = info.st_mtimespec;
#else
    const auto stamp = info.st_mtim;
#endif
    if (stamp.tv_sec >= 0) {
        millis = static_cast<double>(stamp.tv_sec) * 1000.0 + static_cast<double>(stamp.tv_nsec) / 1000000.0;
        valid_time = true;
    }
#endif
    result = Json{{"exists", true}, {"isFile", file}, {"isDirectory", directory}, {"size", size}};
    if (valid_time) {
        result["mtimeMs"] = millis;
        result["mtimeUtc"] = utc_from_millis(static_cast<std::int64_t>(millis));
    }
    return result;
}
std::optional<std::string> optional_text(const Json& value, const char* key) {
    return value.contains(key) && value[key].is_string() ? std::optional(value[key].get<std::string>())
                                                         : std::nullopt;
}
} // namespace
Json filesystem_operation(std::string_view operation, const Json& args) {
    if (operation == "job_files")
        return job_filesystem_operation(args);
    if (operation == "artifact_upload") {
        // The frontend validates its layout and starts the coordinator first.
        // This killable worker may connect, but cannot create a detached owner.
        StateClientOptions options;
        options.start_if_absent = false;
        auto state = open_coordinated_state(path_from_utf8(json_string(args, "state_root")), options);
        ArtifactUploads uploads(state, path_from_utf8(json_string(args, "state_root")) / "artifacts",
                                path_from_utf8(json_string(args, "workspace")));
        const auto& request = args.at("request");
        const auto action = json_string(request, "action"), id = json_string(request, "upload_id");
        if (action == "begin") {
            if (!request.contains("total_bytes") || !request.contains("path"))
                throw Error("Upload begin requires path, total_bytes, sha256 and expected_file_sha256");
            return uploads.begin("operator", id, path_from_utf8(json_string(request, "path")),
                                 json_uint(request, "total_bytes"), json_string(request, "sha256"),
                                 json_string(request, "expected_file_sha256"));
        }
        if (action == "chunk") {
            const auto encoded = json_string(request, "content_base64");
            const auto maximum = json_uint(args, "max_transfer_chars");
            if (!request.contains("offset_bytes") || encoded.size() > 1398104 ||
                (maximum && encoded.size() > maximum))
                throw Error("Upload chunk requires an offset and at most 1 MiB of bytes within the "
                            "configured transfer limit");
            const auto bytes = base64_decode(encoded);
            if (base64_encode(bytes) != encoded)
                throw Error("content_base64 must be canonical");
            return uploads.chunk("operator", id, json_uint(request, "offset_bytes"),
                                 std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                 json_string(request, "sha256"));
        }
        if (action == "finalize")
            return uploads.finalize("operator", id);
        if (action == "cancel")
            return uploads.cancel("operator", id);
        if (action == "status")
            return uploads.status("operator", id);
        throw Error("Unknown artifact upload action");
    }
    if (operation == "legacy_task") {
        const auto root = path_from_utf8(json_string(args, "root"));
        const auto& request = args.at("request");
        const auto action = json_string(args, "action");
        if (action == "get")
            return task_get(root, json_string(request, "task_id"));
        if (action == "put")
            return task_put(root, json_string(request, "task_id"), json_uint(request, "expected_revision"),
                            request.at("state"));
        if (action == "list")
            return task_list(root, optional_text(request, "cursor"), json_uint(request, "limit", 50));
        throw Error("FILESYSTEM_WORKER_TASK_ACTION_INVALID");
    }
    const auto path = path_from_utf8(json_string(args, "path"));
    if (operation == "path_state")
        return path_state(path);
    if (json_uint(args, "max_bytes") > 8 * 1024 * 1024)
        throw Error("FILESYSTEM_WORKER_READ_BUDGET");
    if (operation == "inspect") {
        auto config = std::make_shared<Config>();
        config->platform = Platform::detect();
        config->host_exec_enabled = true;
        config->power_shell_exe = json_string(args, "powershell");
        config->power_shell_fallback_exe = json_string(args, "powershell_fallback");
        RuntimeExecutor runtime(config);
        InspectFileRequest request;
        request.path = json_string(args, "requested_path");
        request.resolved_path = path;
        request.working_dir = path_from_utf8(json_string(args, "working_dir"));
        request.max_bytes = json_uint(args, "max_bytes", 65536);
        return inspect_host_file(*config, runtime, request, {});
    }
    if (operation == "state")
        return file_state(path).json();
    if (operation == "read_text")
        return Json{{"text", read_text(path, json_uint(args, "max_bytes", 65536))}};
    if (operation == "read_large")
        return read_large(path, json_uint(args, "offset_bytes"), json_uint(args, "max_bytes", 262144));
    if (operation == "write_large")
        return write_large(path, json_string(args, "content_base64"), json_bool(args, "append"),
                           json_bool(args, "create_dirs", true), optional_text(args, "expected_sha256"));
    if (operation == "write_text" || operation == "atomic_write") {
        auto content = json_string(args, "content");
        if (operation == "atomic_write") {
            const auto encoded = json_string(args, "content_base64");
            const auto bytes = base64_decode(encoded);
            if (base64_encode(bytes) != encoded)
                throw Error("content_base64 must be canonical");
            content.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        Preconditions expected;
        expected.sha256 = optional_text(args, "expected_file_sha256");
        if (args.contains("expected_offset_bytes") && !args["expected_offset_bytes"].is_null())
            expected.offset = json_uint(args, "expected_offset_bytes");
        return atomic_write(path, content, json_bool(args, "append"), json_bool(args, "create_dirs", true),
                            expected)
            .json();
    }
    if (operation == "list") {
        ListOptions list;
        list.path = path;
        list.recursive = json_bool(args, "recursive");
        list.max_depth = json_uint(args, "max_depth", 4);
        list.max_entries = json_uint(args, "max_entries", 5000);
        list.timeout = Millis(json_uint(args, "timeout_seconds", 30) * 1000);
        list.exclude_directories = json_strings(args, "exclude_directories");
        const auto result = list_files(list);
        return Json{{"stdout", result.stdout_text}, {"stderr", result.stderr_text}};
    }
    throw Error("FILESYSTEM_WORKER_OPERATION_INVALID");
}
int run_filesystem_worker(const FilesystemDispatch& dispatch) {
    try {
        std::string bytes;
        std::vector<char> buffer(16384);
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > 16 * 1024 * 1024 - bytes.size())
                throw Error("FILESYSTEM_WORKER_INPUT_BUDGET");
            bytes.append(buffer.data(), count);
        }
        std::size_t nodes = 0;
        const auto request = Json::parse(bytes, [&](int depth, Json::parse_event_t, Json&) {
            // Preserve the transport's bounded deep-checkpoint contract when
            // adding our internal envelope. Byte limits remain independent.
            if (depth > 256 || ++nodes > 131072)
                throw Error("FILESYSTEM_WORKER_INPUT_SHAPE");
            return true;
        });
        const auto value = (dispatch ? dispatch : filesystem_operation)(json_string(request, "operation"),
                                                                        request.at("arguments"));
        const auto output = bounded_json_dump(Json{{"ok", true}, {"value", value}}, 64 * 1024 * 1024);
        std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
        return 0;
    } catch (const std::exception& error) {
        const auto output = Json{{"ok", false}, {"error", error.what()}}.dump();
        std::cout << output;
        return 0;
    }
}
Json isolated_filesystem(std::string_view operation, const Json& args, Millis timeout, const Cancel& cancel,
                         const fs::path& worker) {
    ProcessOptions options;
    options.input = bounded_json_dump(Json{{"operation", operation}, {"arguments", args}}, 16 * 1024 * 1024);
    options.timeout = std::clamp(timeout, Millis(1), Millis(300000));
    options.termination_grace = Millis(1000);
    // JSON is collected in one bounded byte buffer, not a per-character capture deque.
    options.max_capture_chars = 0;
    std::string output;
    bool output_overflow = false;
    options.on_output = [&](OutputStream stream, std::string_view bytes) {
        if (stream != OutputStream::stdout_stream)
            return;
        if (bytes.size() > 64 * 1024 * 1024 - output.size()) {
            output_overflow = true;
            throw Error("FILESYSTEM_WORKER_OUTPUT_BUDGET");
        }
        output.append(bytes);
    };
    options.env = worker_environment();
#ifdef _WIN32
    options.memory_limit_bytes = 256 * 1024 * 1024;
    options.process_limit = operation == "inspect" ? 2 : 1;
#endif
    const auto binary = worker.empty() ? executable_path() : worker;
    try {
        (void)spawn_process(path_text(binary), {"--filesystem-worker"}, options, cancel);
        std::size_t nodes = 0;
        const auto response = Json::parse(output, [&](int depth, Json::parse_event_t, Json&) {
            if (depth > 256 || ++nodes > 131072)
                throw Error("FILESYSTEM_WORKER_OUTPUT_SHAPE");
            return true;
        });
        if (!json_bool(response, "ok"))
            throw Error(json_string(response, "error", "Filesystem worker failed"));
        return response.at("value");
    } catch (const ProcessError& error) {
        if (output_overflow)
            throw Error("FILESYSTEM_WORKER_OUTPUT_BUDGET");
        if (error.aborted)
            throw Cancelled();
        if (error.timed_out)
            throw FilesystemDeadline();
        throw Error("FILESYSTEM_WORKER_FAILED: inspect destination version before retrying a write");
    }
}
} // namespace devbox
