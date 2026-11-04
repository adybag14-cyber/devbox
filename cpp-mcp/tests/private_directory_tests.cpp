#include "devbox/native.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#ifdef _WIN32
#include <aclapi.h>
#include <winioctl.h>
#endif
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class Fn> void rejects(Fn operation, std::string_view expected) {
    try {
        operation();
    } catch (const Error& error) {
        if (std::string_view(error.what()).find(expected) != std::string::npos)
            return;
        throw;
    }
    throw Error("Private-directory guard accepted a prohibited path");
}
#ifdef _WIN32
fs::path extended(const fs::path& path) {
    return fs::path(std::wstring(L"\\\\?\\") + path.native());
}
void junction(const fs::path& link, const fs::path& target) {
    ensure_directory(link);
    struct MountPoint {
        DWORD tag;
        WORD bytes, reserved, substitute_offset, substitute_bytes, print_offset, print_bytes;
        wchar_t paths[1024];
    } data{};
    const auto substitute = std::wstring(L"\\??\\") + target.native();
    const auto printed = target.native();
    require(substitute.size() + printed.size() + 2 < std::size(data.paths), "Bounded junction fixture");
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_bytes = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_bytes = static_cast<WORD>(printed.size() * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    std::copy(printed.begin(), printed.end(), data.paths + substitute.size() + 1);
    data.bytes = static_cast<WORD>(8 + (substitute.size() + printed.size() + 2) * sizeof(wchar_t));
    NativeHandle handle(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    DWORD returned = 0;
    require(handle && DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, &data, data.bytes + 8, nullptr,
                                      0, &returned, nullptr),
            "Create private junction fixture without changing volume mounts");
}
void acl_recheck(const fs::path& directory) {
    PACL original = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, &original, nullptr, &descriptor) == ERROR_SUCCESS,
            "Read private fixture DACL");
    ScopeExit release([&] { LocalFree(descriptor); });
    alignas(DWORD) std::array<unsigned char, SECURITY_MAX_SID_SIZE> world{};
    DWORD length = static_cast<DWORD>(world.size());
    require(CreateWellKnownSid(WinWorldSid, nullptr, world.data(), &length), "Create fixture Everyone SID");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_GENERIC_READ;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&access.Trustee, world.data());
    PACL expanded = nullptr;
    require(SetEntriesInAclW(1, &access, original, &expanded) == ERROR_SUCCESS, "Build widened fixture DACL");
    ScopeExit free_expanded([&] { LocalFree(expanded); });
    const auto set = [&](PACL acl) {
        return SetNamedSecurityInfoW(const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                     nullptr, acl, nullptr) == ERROR_SUCCESS;
    };
    require(set(expanded), "Apply widened DACL only to the owned fixture");
    ScopeExit restore([&] { (void)set(original); });
    rejects([&] { ensure_private_state_directory(directory); }, "STATE_PRIVATE_ACL_REQUIRED");
    require(set(original), "Restore private fixture DACL");
    restore.disarm();
    ensure_private_state_directory(directory);
}
#endif
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-private-path-" + uuid());
    ensure_directory(root);
    ScopeExit cleanup([&] {
        std::error_code error;
#ifdef _WIN32
        fs::remove_all(extended(root), error);
#else
        fs::remove_all(root, error);
#endif
    });
    try {
        const auto target = root / "private";
        ensure_private_state_directory(target);
        ensure_private_state_directory(target);
        rejects([&] { ensure_private_state_directory(fs::path("relative-private")); },
                "STATE_LOCAL_PATH_REQUIRED");
        const auto ordinary = root / "ordinary-file";
        write_file(ordinary, "preserve");
        rejects([&] { ensure_private_state_directory(ordinary); }, "STATE_PRIVATE_DIRECTORY_REQUIRED");
#ifdef _WIN32
        ensure_private_state_directory(extended(target));
        rejects([&] { ensure_private_state_directory(target, true); }, "STATE_DIRECTORY_EXISTS");
        for (const auto* path : {L"\\\\example.invalid\\share\\private",
                                 L"\\\\?\\UNC\\example.invalid\\share\\private", L"\\\\.\\C:\\private"})
            rejects([&] { ensure_private_state_directory(fs::path(path)); },
                    "STATE_NETWORK_OR_DEVICE_PATH_DENIED");
        const auto link = root / "junction";
        junction(link, target);
        ScopeExit remove_link([&] {
            std::error_code error;
            fs::remove(link, error);
        });
        rejects([&] { ensure_private_state_directory(link); }, "STATE_PRIVATE_DIRECTORY_REQUIRED");
        rejects([&] { ensure_private_state_directory(link / "child"); }, "STATE_REPARSE_PARENT_DENIED");
        rejects([&] { ensure_private_state_directory(extended(link / "child")); },
                "STATE_REPARSE_PARENT_DENIED");
        require(!fs::exists(target / "child"), "Rejected junction traversal never creates a child");
        acl_recheck(target);
        auto deep = root;
        while (deep.native().size() < 300)
            deep /= std::string(48, 'd');
        fs::create_directories(extended(deep));
        ensure_private_state_directory(extended(deep / "private"));
        ensure_private_state_directory(extended(deep / "private"));
#else
        rejects([&] { ensure_private_state_directory(target, true); }, "STATE_DIRECTORY_CREATE_FAILED");
        fs::permissions(target, fs::perms::group_read, fs::perm_options::add);
        rejects([&] { ensure_private_state_directory(target); }, "STATE_PRIVATE_DIRECTORY_REQUIRED");
        fs::permissions(target, fs::perms::group_read, fs::perm_options::remove);
        ensure_private_state_directory(target);
#endif
        std::cout << "Private directory roots, reparse boundaries and fresh permissions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
