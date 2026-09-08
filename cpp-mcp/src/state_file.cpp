#include "devbox/common.hpp"
#include "devbox/native.hpp"
#include <cstring>
#include <thread>
namespace devbox {
void replace_state_file(const fs::path& source, const fs::path& target) {
#ifdef _WIN32
    // Windows 10 POSIX rename semantics keep readers of the previous journal valid.
    // See Microsoft's FILE_RENAME_INFORMATION and FILE_RENAME_INFO contracts.
    const auto destination = fs::absolute(target).wstring();
    const auto name_bytes = destination.size() * sizeof(wchar_t);
    if (name_bytes > MAXDWORD - sizeof(FILE_RENAME_INFO))
        throw Error("State file path is too long");
    const auto size = sizeof(FILE_RENAME_INFO) + name_bytes;
    std::vector<unsigned char> storage(size, 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(info->FileName, destination.data(), name_bytes);
    NativeHandle file(CreateFileW(source.c_str(), DELETE | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        throw Error("open state replacement: " + windows_error());
    if (SetFileInformationByHandle(file.get(), FileRenameInfoEx, info, static_cast<DWORD>(size))) {
        if (!FlushFileBuffers(file.get()))
            throw Error("flush state replacement: " + windows_error());
        return;
    }
    const auto error = GetLastError();
    file.reset();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION)
        throw Error("replace state: " + windows_error(error));
    // Older/remote filesystems can use the classic operation, with a bounded sharing retry.
    const auto deadline = Clock::now() + Millis(1000);
    while (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto fallback_error = GetLastError();
        if ((fallback_error != ERROR_ACCESS_DENIED && fallback_error != ERROR_SHARING_VIOLATION) ||
            Clock::now() >= deadline)
            throw Error("replace state: " + windows_error(fallback_error));
        std::this_thread::sleep_for(Millis(5));
    }
#else
    fs::rename(source, target);
#endif
}
} // namespace devbox
