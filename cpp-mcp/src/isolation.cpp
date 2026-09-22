#include "devbox/isolation.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#ifdef _WIN32
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace devbox {
namespace {
bool beneath(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && relative != "." &&
           std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
}
void no_links(const fs::path& path) {
    auto cursor = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (part == "." || part == "..")
            throw Error("ISOLATION_PATH_ALIAS_DENIED");
        cursor /= part;
        if (fs::is_symlink(fs::symlink_status(cursor)))
            throw Error("ISOLATION_PATH_ALIAS_DENIED");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error("ISOLATION_PATH_ALIAS_DENIED");
#endif
    }
}
#ifdef _WIN32
class PinnedDirectories {
    std::vector<NativeHandle> handles_;

  public:
    explicit PinnedDirectories(const fs::path& leaf) {
        auto cursor = leaf.root_path();
        for (const auto& part : leaf.relative_path()) {
            cursor /= part;
            NativeHandle handle(CreateFileW(cursor.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                            nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!handle || !GetFileInformationByHandle(handle.get(), &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw Error("ISOLATION_DIRECTORY_PIN_FAILED");
            handles_.push_back(std::move(handle));
        }
    }
};
class ScopedAccess {
    fs::path path_;
    PSECURITY_DESCRIPTOR original_ = nullptr;
    PACL original_acl_ = nullptr;
    bool changed_ = false;

  public:
    ScopedAccess(const fs::path& path, PSID sid, DWORD access) : path_(path) {
        if (GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                  &original_acl_, nullptr, &original_) != ERROR_SUCCESS ||
            !original_acl_) {
            if (original_)
                LocalFree(original_);
            original_ = nullptr;
            throw Error("ISOLATION_ACL_READ_FAILED");
        }
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = access;
        entry.grfAccessMode = GRANT_ACCESS;
        entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL next = nullptr;
        if (SetEntriesInAclW(1, &entry, original_acl_, &next) != ERROR_SUCCESS) {
            LocalFree(original_);
            original_ = nullptr;
            throw Error("ISOLATION_ACL_CONSTRUCTION_FAILED");
        }
        ScopeExit release([&] { LocalFree(next); });
        const auto status = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, next, nullptr);
        if (status != ERROR_SUCCESS) {
            LocalFree(original_);
            original_ = nullptr;
            throw Error("ISOLATION_ACL_GRANT_FAILED");
        }
        changed_ = true;
    }
    void restore() {
        if (changed_ && SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
                                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                              nullptr, nullptr, original_acl_, nullptr) != ERROR_SUCCESS)
            throw Error("ISOLATION_ACL_RESTORE_FAILED");
        changed_ = false;
    }
    ~ScopedAccess() {
        try {
            restore();
        } catch (...) {
        }
        if (original_)
            LocalFree(original_);
    }
};
#endif
} // namespace
Json isolation_capabilities() {
#ifdef _WIN32
    return Json{{"profile", "windows_lpac"},
                {"status", "available"},
                {"network", "denied"},
                {"desktop_input", "denied"},
                {"child_processes", "denied"},
                {"registry", "registryRead_capability_acls"},
                {"program_compatibility", "must_support_win32k_lockdown"},
                {"max_captured_chars_per_stream", 4096},
                {"workspace", "private_run_workspace"},
                {"enforced_only_after_successful_launch", true}};
#elif defined(__linux__) && !defined(__ANDROID__)
    return Json{{"profile", "linux_bubblewrap"},
                {"status", "unsupported"},
                {"reason", "Linux isolation backend has not yet been qualified"}};
#else
    return Json{{"profile", "none"},
                {"status", "unsupported"},
                {"reason", "No qualified autonomous OS isolation backend for this platform"}};
#endif
}
ProcessOutput run_isolated_program(const IsolatedProgram& request, const Cancel& cancel) {
#ifndef _WIN32
    (void)request;
    (void)cancel;
    throw Error("ISOLATION_UNSUPPORTED: this platform cannot execute autonomous grants yet");
#else
    if (request.timeout <= Millis(0) || request.timeout > Millis(3600000) ||
        request.memory_bytes < 64 * 1024 * 1024 || request.memory_bytes > 8ULL * 1024 * 1024 * 1024 ||
        !request.cpu_ms || request.cpu_ms > 3600000 || request.output_chars > 4096 ||
        request.arguments.size() > 256 || (request.input && request.input->size() > 65536))
        throw Error("ISOLATION_INVALID_BUDGET");
    if (!request.private_root.is_absolute() || !request.workspace.is_absolute() ||
        !request.executable.is_absolute())
        throw Error("ISOLATION_ABSOLUTE_PATH_REQUIRED");
    no_links(request.private_root);
    no_links(request.workspace);
    no_links(request.executable);
    ensure_private_state_directory(request.private_root);
    const auto root = fs::canonical(request.private_root), workspace = fs::canonical(request.workspace);
    if (!beneath(workspace, root))
        throw Error("ISOLATION_PRIVATE_WORKSPACE_REQUIRED");
    ensure_private_state_directory(workspace);
    FileLock workspace_lease(root / (sha256(path_text(workspace)) + ".execution.lock"), Millis(1000), cancel,
                             true);
    PinnedDirectories pinned_workspace(workspace), pinned_executable_parent(request.executable.parent_path());
    NativeHandle executable(CreateFileW(request.executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!executable || !GetFileInformationByHandle(executable.get(), &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw Error("ISOLATION_EXECUTABLE_PIN_FAILED");
    if (fs::file_size(request.executable) > 256 * 1024 * 1024 ||
        sha256_file(request.executable) != request.executable_sha256)
        throw Error("ISOLATION_EXECUTABLE_IDENTITY_CHANGED");
    const auto runtime = root / ("worker-" + uuid());
    ensure_private_state_directory(runtime);
    const auto image = runtime / "program.exe";
    fs::copy_file(request.executable, image, fs::copy_options::none);
    if (sha256_file(image) != request.executable_sha256)
        throw Error("ISOLATION_EXECUTABLE_COPY_MISMATCH");
    const auto name = L"Devbox.Worker." + wide(uuid());
    PSID sid = nullptr;
    const auto created =
        CreateAppContainerProfile(name.c_str(), L"Devbox isolated worker",
                                  L"Temporary identity for one admitted Devbox operation", nullptr, 0, &sid);
    if (FAILED(created))
        throw Error("ISOLATION_PERMISSION_DENIED: cannot create a worker AppContainer");
    ScopeExit release_sid([&] { FreeSid(sid); });
    bool profile_exists = true;
    ScopeExit remove_profile([&] {
        if (profile_exists)
            DeleteAppContainerProfile(name.c_str());
    });
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(sid, &sid_text))
        throw Error("ISOLATION_IDENTITY_FAILED");
    ScopeExit release_sid_text([&] { LocalFree(sid_text); });
    // The ordinary host account keeps its existing authority. Only the temporary package SID
    // gains access to this run's workspace and the immutable executable copy.
    ScopedAccess workspace_access(workspace, sid,
                                  FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE);
    ScopedAccess image_access(runtime, sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    write_json_atomic(runtime / "owner.json", Json{{"profile", narrow(name)},
                                                   {"pid", process_id()},
                                                   {"instance", process_instance(process_id()).value_or(0)},
                                                   {"status", "prepared"}});
    ProcessOptions options;
    options.cwd = workspace;
    options.env = worker_environment();
    (*options.env)["TEMP"] = path_text(workspace);
    (*options.env)["TMP"] = path_text(workspace);
    options.timeout = request.timeout;
    options.max_capture_chars = request.output_chars;
    options.memory_limit_bytes = request.memory_bytes;
    options.cpu_limit_ms = request.cpu_ms;
    options.process_limit = 1;
    options.appcontainer_sid = narrow(sid_text);
    options.input = request.input;
    if (request.transition_hook)
        request.transition_hook("before_launch");
    std::optional<ProcessOutput> result;
    std::exception_ptr failure;
    try {
        result = spawn_process(path_text(image), request.arguments, options, cancel);
    } catch (...) {
        failure = std::current_exception();
    }
    workspace_access.restore();
    image_access.restore();
    if (FAILED(DeleteAppContainerProfile(name.c_str())))
        throw Error("ISOLATION_PROFILE_CLEANUP_UNCONFIRMED");
    profile_exists = false;
    write_json_atomic(runtime / "owner.json", Json{{"profile", narrow(name)}, {"status", "retired"}});
    // Only broker-created files are removed. Untrusted workspace contents are retained as artifacts.
    fs::remove(image);
    fs::remove(runtime / "owner.json");
    fs::remove(runtime);
    if (failure)
        std::rethrow_exception(failure);
    return *result;
#endif
}
} // namespace devbox
