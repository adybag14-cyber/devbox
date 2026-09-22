#include "devbox/scheduler_notifications.hpp"
#include "devbox/native.hpp"
#include "devbox/scoped_thread.hpp"
#include "devbox/state_store.hpp"
#include <algorithm>
#include <array>
#include <future>
#include <map>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif
#endif
namespace devbox {
struct SchedulerNotifications::State {
    mutable std::mutex mutex;
    std::mutex write_mutex;
    Cancel changed = std::make_shared<Cancellation>();
    std::atomic_bool available{false};
    std::atomic<std::uint64_t> notices{0}, failures{0};
    fs::path root;
    NativeHandle directory, file, events;
#ifdef _WIN32
    NativeHandle stop;
#endif
    ScopedThread worker;
    void notify() noexcept {
        try {
            auto next = std::make_shared<Cancellation>();
            {
                std::lock_guard lock(mutex);
                changed.swap(next);
            }
            ++notices;
            next->cancel();
        } catch (...) {
            ++failures;
        }
    }
    explicit State(fs::path value) : root(std::move(value) / ".notifications") {
        ensure_directory(root.parent_path());
        ensure_private_state_directory(root);
        const auto signal_path = root / ".changed";
#ifdef _WIN32
        directory.reset(CreateFileW(
            root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        file.reset(CreateFileW(signal_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!directory || !file || GetFileType(file.get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(file.get(), &info) || info.nNumberOfLinks != 1 ||
            info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            throw Error("SCHEDULER_NOTIFICATION_FILE_REJECTED");
        events.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!events || !stop)
            throw Error(windows_error());
#else
        directory.reset(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!directory)
            throw Error("SCHEDULER_NOTIFICATION_DIRECTORY_REJECTED");
        file.reset(::openat(directory.get(), ".changed", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        struct stat info{};
        const auto opened_error = file ? 0 : errno;
        const auto stat_error = file && ::fstat(file.get(), &info) ? errno : 0;
        if (!file || stat_error || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
            info.st_uid != ::geteuid() || (info.st_mode & 077))
            throw Error("SCHEDULER_NOTIFICATION_FILE_REJECTED: open=" + std::to_string(opened_error) +
                        " stat=" + std::to_string(stat_error) + " mode=" + std::to_string(info.st_mode) +
                        " links=" + std::to_string(info.st_nlink) + " owner=" + std::to_string(info.st_uid) +
                        " caller=" + std::to_string(::geteuid()));
#if defined(__linux__)
        events.reset(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
        if (!events || ::inotify_add_watch(events.get(), signal_path.c_str(),
                                           IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF) < 0)
            throw Error("SCHEDULER_NOTIFICATION_WATCH_UNAVAILABLE");
#elif defined(__APPLE__)
        events.reset(::kqueue());
        if (!events)
            throw Error("SCHEDULER_NOTIFICATION_WATCH_UNAVAILABLE");
        ::fcntl(events.get(), F_SETFD, FD_CLOEXEC);
        struct kevent event{};
        EV_SET(&event, file.get(), EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME, 0,
               nullptr);
        if (::kevent(events.get(), &event, 1, nullptr, 0, nullptr))
            throw Error("SCHEDULER_NOTIFICATION_WATCH_UNAVAILABLE");
#endif
#endif
        auto ready = std::make_shared<std::promise<void>>();
        auto future = ready->get_future();
        worker = ScopedThread([this, ready](ThreadStopToken stopping) {
            try {
#ifdef _WIN32
                alignas(DWORD) std::array<unsigned char, 8192> buffer{};
                bool first = true;
                while (!stopping.stop_requested()) {
                    OVERLAPPED operation{};
                    operation.hEvent = events.get();
                    ResetEvent(events.get());
                    if (!ReadDirectoryChangesW(directory.get(), buffer.data(),
                                               static_cast<DWORD>(buffer.size()), FALSE,
                                               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
                                               nullptr, &operation, nullptr))
                        throw Error("SCHEDULER_NOTIFICATION_WATCH_FAILED");
                    if (first) {
                        available = true;
                        ready->set_value();
                        first = false;
                    }
                    HANDLE handles[]{stop.get(), events.get()};
                    const auto signalled = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                    if (signalled != WAIT_OBJECT_0 + 1) {
                        CancelIoEx(directory.get(), &operation);
                        DWORD ignored = 0;
                        GetOverlappedResult(directory.get(), &operation, &ignored, TRUE);
                        break;
                    }
                    DWORD bytes = 0;
                    if (!GetOverlappedResult(directory.get(), &operation, &bytes, FALSE))
                        break;
                    bool changed_file = bytes == 0; // overflow: reconcile, never infer state from an event
                    std::size_t offset = 0;
                    while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
                        const auto* item =
                            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                        if (item->FileNameLength >
                            bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName))
                            break;
                        if (std::wstring_view(item->FileName, item->FileNameLength / sizeof(wchar_t)) ==
                            L".changed")
                            changed_file = true;
                        if (!item->NextEntryOffset || item->NextEntryOffset > bytes - offset ||
                            item->NextEntryOffset % alignof(DWORD))
                            break;
                        offset += item->NextEntryOffset;
                    }
                    if (changed_file)
                        notify();
                }
#elif defined(__linux__)
                available = true;
                ready->set_value();
                while (!stopping.stop_requested()) {
                    pollfd descriptor{events.get(), POLLIN, 0};
                    if (::poll(&descriptor, 1, 100) > 0) {
                        std::array<char, 4096> buffer{};
                        if (::read(events.get(), buffer.data(), buffer.size()) > 0)
                            notify();
                    }
                }
#elif defined(__APPLE__)
                available = true;
                ready->set_value();
                while (!stopping.stop_requested()) {
                    struct kevent event{};
                    const timespec timeout{0, 100000000};
                    if (::kevent(events.get(), nullptr, 0, &event, 1, &timeout) > 0)
                        notify();
                }
#else
                ready->set_value();
#endif
            } catch (...) {
                ++failures;
                try {
                    ready->set_value();
                } catch (...) {
                }
            }
            available = false;
        });
        future.get();
    }
    ~State() {
        worker.request_stop();
#ifdef _WIN32
        SetEvent(stop.get());
#else
        // Wake the descriptor wait immediately; the notification remains only a reconciliation hint.
        if (file)
            (void)::pwrite(file.get(), "!", 1, 0);
#endif
        worker.join();
    }
    void signal() noexcept {
        notify();
        std::lock_guard lock(write_mutex);
#ifdef _WIN32
        // LAST_WRITE notifications are delivered when the writer closes. Keep the pinned handle
        // read-only and close each short-lived writer; no fsync/durable message is needed for a wake hint.
        NativeHandle publisher(CreateFileW((root / ".changed").c_str(), GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION held{}, opened{};
        DWORD written = 0;
        if (!publisher || !GetFileInformationByHandle(file.get(), &held) ||
            !GetFileInformationByHandle(publisher.get(), &opened) || opened.nNumberOfLinks != 1 ||
            held.dwVolumeSerialNumber != opened.dwVolumeSerialNumber ||
            held.nFileIndexHigh != opened.nFileIndexHigh || held.nFileIndexLow != opened.nFileIndexLow ||
            !WriteFile(publisher.get(), "!", 1, &written, nullptr) || written != 1)
            ++failures;
#else
        if (::pwrite(file.get(), "!", 1, 0) != 1)
            ++failures;
#endif
    }
};
SchedulerNotifications::SchedulerNotifications(const fs::path& root)
    : state_(std::make_unique<State>(root)) {}
SchedulerNotifications::~SchedulerNotifications() = default;
Cancel SchedulerNotifications::token() const {
    std::lock_guard lock(state_->mutex);
    return state_->changed;
}
void SchedulerNotifications::signal() noexcept {
    state_->signal();
}
Json SchedulerNotifications::snapshot() const {
    return Json{{"available", state_->available.load()},
                {"notices", state_->notices.load()},
                {"failures", state_->failures.load()},
                {"authority", "owner_only_local_directory"},
                {"reconciliation_required", true}};
}
std::shared_ptr<SchedulerNotifications> scheduler_notifications(const fs::path& root) {
    static std::mutex mutex;
    static std::map<fs::path, std::weak_ptr<SchedulerNotifications>> hubs;
    std::lock_guard lock(mutex);
    const auto path = fs::absolute(root).lexically_normal();
    if (auto existing = hubs[path].lock())
        return existing;
    std::erase_if(hubs, [](const auto& item) { return item.second.expired(); });
    if (hubs.size() >= 64)
        throw Error("SCHEDULER_NOTIFICATION_ROOT_BUDGET");
    auto result = std::make_shared<SchedulerNotifications>(path);
    hubs[path] = result;
    return result;
}
} // namespace devbox
