#pragma once
#include "common.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace devbox {
#ifdef _WIN32
class NativeHandle {
    HANDLE value_ = nullptr;

  public:
    NativeHandle() = default;
    explicit NativeHandle(HANDLE value) : value_(value) {}
    ~NativeHandle() {
        reset();
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(other.release()) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.release();
        }
        return *this;
    }
    HANDLE get() const {
        return value_;
    }
    explicit operator bool() const {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        auto value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) {
        if (*this)
            CloseHandle(value_);
        value_ = value;
    }
};
inline std::wstring wide(std::string_view value) {
    const auto text = to_utf16(value);
    return std::wstring(text.begin(), text.end());
}
inline std::string narrow(std::wstring_view value) {
    return from_utf16(std::u16string(value.begin(), value.end()));
}
std::string windows_error(unsigned long code = GetLastError());
#else
class NativeHandle {
    int value_ = -1;

  public:
    NativeHandle() = default;
    explicit NativeHandle(int value) : value_(value) {}
    ~NativeHandle() {
        reset();
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    NativeHandle(NativeHandle&& other) noexcept : value_(other.release()) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.release();
        }
        return *this;
    }
    int get() const {
        return value_;
    }
    explicit operator bool() const {
        return value_ >= 0;
    }
    int release() {
        auto value = value_;
        value_ = -1;
        return value;
    }
    void reset(int value = -1) {
        if (*this)
            ::close(value_);
        value_ = value;
    }
};
#endif
class ScopeExit {
    std::function<void()> fn_;

  public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        if (fn_) {
            try {
                fn_();
            } catch (...) {
            }
        }
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    void disarm() {
        fn_ = {};
    }
};
} // namespace devbox
