#include "devbox/common.hpp"
#include "devbox/native.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace devbox {
void Cancellation::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
    condition_.notify_all();
}
bool Cancellation::wait_for(Millis duration) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, duration, [this] { return cancelled(); });
}
std::string trim(std::string_view value) {
    const auto a = value.find_first_not_of(" \t\r\n");
    if (a == value.npos)
        return {};
    const auto b = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(a, b - a + 1));
}
std::string lower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return value;
}
bool starts_with(std::string_view value, std::string_view prefix) {
    return value.starts_with(prefix);
}
std::vector<std::string> split(std::string_view value, char separator, bool keep_empty) {
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        auto b = value.find(separator, a);
        auto piece = value.substr(a, b == value.npos ? value.size() - a : b - a);
        if (keep_empty || !piece.empty())
            out.emplace_back(piece);
        if (b == value.npos)
            break;
        a = b + 1;
    }
    return out;
}
std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::string out;
    bool first = true;
    for (const auto& value : values) {
        if (!first)
            out += separator;
        out += value;
        first = false;
    }
    return out;
}
std::string replace_all(std::string value, std::string_view from, std::string_view to) {
    if (from.empty())
        return value;
    std::size_t p = 0;
    while ((p = value.find(from, p)) != value.npos) {
        value.replace(p, from.size(), to);
        p += to.size();
    }
    return value;
}

namespace {
std::uint32_t next_rune(std::string_view value, std::size_t& index) {
    const auto first = static_cast<unsigned char>(value[index++]);
    if (first < 0x80)
        return first;
    unsigned count = first >= 0xC2 && first <= 0xDF   ? 1
                     : first >= 0xE0 && first <= 0xEF ? 2
                     : first >= 0xF0 && first <= 0xF4 ? 3
                                                      : 0;
    if (!count)
        return 0xFFFD;
    std::uint32_t rune = first & ((1U << (6 - count)) - 1U);
    for (unsigned n = 0; n < count; ++n) {
        if (index == value.size())
            return 0xFFFD;
        const auto c = static_cast<unsigned char>(value[index]);
        if (c < 0x80 || c > 0xBF)
            return 0xFFFD;
        if (n == 0 && ((first == 0xE0 && c < 0xA0) || (first == 0xED && c >= 0xA0) ||
                       (first == 0xF0 && c < 0x90) || (first == 0xF4 && c >= 0x90)))
            return 0xFFFD;
        ++index;
        rune = (rune << 6U) | (c & 0x3FU);
    }
    return rune;
}
void append_utf8(std::string& out, std::uint32_t c) {
    if (c < 0x80)
        out.push_back(static_cast<char>(c));
    else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 63)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (c & 63)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (c & 63)));
    }
}
int digits(std::string_view s) {
    int value = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
        throw Error("Invalid timestamp");
    return value;
}
struct Digest {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    Digest() {
        if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
            throw Error("SHA-256 initialization failed");
    }
    ~Digest() {
        EVP_MD_CTX_free(context);
    }
    void update(const void* data, std::size_t size) {
        if (EVP_DigestUpdate(context, data, size) != 1)
            throw Error("SHA-256 update failed");
    }
    std::string finish() {
        std::array<std::uint8_t, EVP_MAX_MD_SIZE> bytes{};
        unsigned size = 0;
        if (EVP_DigestFinal_ex(context, bytes.data(), &size) != 1)
            throw Error("SHA-256 finalization failed");
        return hex(std::span(bytes.data(), size));
    }
};
} // namespace
std::u16string to_utf16(std::string_view value) {
    std::u16string out;
    for (std::size_t i = 0; i < value.size();) {
        auto c = next_rune(value, i);
        if (c < 0x10000)
            out.push_back(static_cast<char16_t>(c));
        else {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 1023)));
        }
    }
    return out;
}
std::string from_utf16(std::u16string_view value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::uint32_t c = value[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < value.size() && value[i + 1] >= 0xDC00 && value[i + 1] <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (value[++i] - 0xDC00);
            } else
                c = 0xFFFD;
        } else if (c >= 0xDC00 && c <= 0xDFFF)
            c = 0xFFFD;
        append_utf8(out, c);
    }
    return out;
}
std::string sanitize_utf8(std::string_view value) {
    return from_utf16(to_utf16(value));
}
std::size_t js_length(std::string_view value) {
    return to_utf16(value).size();
}
std::string js_slice(std::string_view value, std::size_t start, std::size_t end) {
    const auto units = to_utf16(value);
    start = std::min(start, units.size());
    end = std::max(start, std::min(end, units.size()));
    return from_utf16(std::u16string_view(units).substr(start, end - start));
}
fs::path path_from_utf8(std::string_view value) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string path_text(const fs::path& value) {
    const auto s = value.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}
std::optional<std::string> environment(std::string_view name) {
#ifdef _WIN32
    const auto key = to_utf16(name);
    const auto wide = reinterpret_cast<const wchar_t*>(key.c_str());
    SetLastError(ERROR_SUCCESS);
    const auto size = GetEnvironmentVariableW(wide, nullptr, 0);
    if (!size) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return std::nullopt;
        return std::string{};
    }
    std::wstring value(size, L'\0');
    const auto n = GetEnvironmentVariableW(wide, value.data(), size);
    return from_utf16({reinterpret_cast<const char16_t*>(value.data()), n});
#else
    const auto* value = std::getenv(std::string(name).c_str());
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}
void set_environment(std::string_view name, const std::optional<std::string>& value) {
#ifdef _WIN32
    const auto key = to_utf16(name);
    const auto val = value ? to_utf16(*value) : std::u16string{};
    if (!SetEnvironmentVariableW(reinterpret_cast<const wchar_t*>(key.c_str()),
                                 value ? reinterpret_cast<const wchar_t*>(val.c_str()) : nullptr))
        throw Error("Could not set environment variable");
#else
    const auto rc =
        value ? setenv(std::string(name).c_str(), value->c_str(), 1) : unsetenv(std::string(name).c_str());
    if (rc)
        throw Error("Could not set environment variable");
#endif
}
std::string env_or(std::string_view name, std::string_view fallback) {
    return environment(name).value_or(std::string(fallback));
}
bool env_bool(std::string_view name, bool fallback) {
    const auto v = environment(name);
    if (!v)
        return fallback;
    const auto s = lower(trim(*v));
    if (s.empty())
        return fallback;
    return s == "true" || s == "1" || s == "yes" || s == "on";
}
std::uint64_t env_uint(std::string_view name, std::uint64_t fallback) {
    const auto v = environment(name);
    if (!v)
        return fallback;
    const auto s = trim(*v);
    std::uint64_t out = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() ? out : fallback;
}
std::uint64_t unix_millis() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<Millis>(std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string utc_from_millis(std::int64_t ms) {
    auto seconds = ms / 1000;
    auto remainder = ms % 1000;
    if (remainder < 0) {
        --seconds;
        remainder += 1000;
    }
    const auto t = static_cast<std::time_t>(seconds);
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value, &t);
#else
    gmtime_r(&t, &value);
#endif
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << remainder
        << 'Z';
    return out.str();
}
std::string utc_now() {
    return utc_from_millis(static_cast<std::int64_t>(unix_millis()));
}
std::optional<std::int64_t> parse_utc(std::string_view value) {
    try {
        if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
            (value[10] != 'T' && value[10] != 't' && value[10] != ' '))
            return std::nullopt;
        const std::chrono::year_month_day date{
            std::chrono::year{digits(value.substr(0, 4))},
            std::chrono::month{static_cast<unsigned>(digits(value.substr(5, 2)))},
            std::chrono::day{static_cast<unsigned>(digits(value.substr(8, 2)))}};
        if (!date.ok())
            return std::nullopt;
        const int h = digits(value.substr(11, 2)), m = digits(value.substr(14, 2)),
                  s = digits(value.substr(17, 2));
        if (h > 23 || m > 59 || s > 60)
            return std::nullopt;
        std::int64_t result =
            std::chrono::duration_cast<Millis>(std::chrono::sys_days(date).time_since_epoch()).count() +
            (h * 3600LL + m * 60LL + s) * 1000;
        std::size_t p = 19;
        if (p < value.size() && value[p] == '.') {
            ++p;
            int place = 100;
            while (p < value.size() && value[p] >= '0' && value[p] <= '9') {
                if (place) {
                    result += (value[p] - '0') * place;
                    place /= 10;
                }
                ++p;
            }
        }
        if (p < value.size() && (value[p] == 'Z' || value[p] == 'z'))
            return p + 1 == value.size() ? std::optional(result) : std::nullopt;
        if (p + 6 == value.size() && (value[p] == '+' || value[p] == '-') && value[p + 3] == ':') {
            const auto hours = digits(value.substr(p + 1, 2)), minutes = digits(value.substr(p + 4, 2));
            if (hours > 23 || minutes > 59)
                return std::nullopt;
            const auto offset = (hours * 60LL + minutes) * 60000;
            return result + (value[p] == '+' ? -offset : offset);
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}
std::vector<std::uint8_t> random_bytes(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error("Random buffer too large");
    std::vector<std::uint8_t> out(size);
    if (size && RAND_bytes(out.data(), static_cast<int>(size)) != 1)
        throw Error("Operating-system random source failed");
    return out;
}
std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out += alphabet[b >> 4];
        out += alphabet[b & 15];
    }
    return out;
}
std::string uuid() {
    auto b = random_bytes(16);
    b[6] = static_cast<std::uint8_t>((b[6] & 15) | 64);
    b[8] = static_cast<std::uint8_t>((b[8] & 63) | 128);
    const auto s = hex(b);
    return s.substr(0, 8) + '-' + s.substr(8, 4) + '-' + s.substr(12, 4) + '-' + s.substr(16, 4) + '-' +
           s.substr(20);
}
std::string sha256(std::span<const std::uint8_t> bytes) {
    Digest digest;
    digest.update(bytes.data(), bytes.size());
    return digest.finish();
}
std::string sha256(std::string_view bytes) {
    return sha256({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
std::string sha256_file(const fs::path& path, std::optional<std::uint64_t> limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw Error("Cannot open file for SHA-256: " + path_text(path));
    Digest digest;
    std::array<char, 65536> block{};
    std::uint64_t read = 0;
    while (file) {
        auto size = limit ? std::min<std::uint64_t>(block.size(), *limit - read) : block.size();
        if (!size)
            break;
        file.read(block.data(), static_cast<std::streamsize>(size));
        const auto n = file.gcount();
        if (n > 0) {
            digest.update(block.data(), static_cast<std::size_t>(n));
            read += static_cast<std::uint64_t>(n);
        }
    }
    if (file.bad())
        throw Error("SHA-256 file read failed");
    return digest.finish();
}
std::string base64_encode(std::span<const std::uint8_t> bytes, bool url) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error("Base64 input too large");
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    if (!bytes.empty())
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), bytes.data(),
                        static_cast<int>(bytes.size()));
    if (url) {
        for (auto& c : out) {
            if (c == '+')
                c = '-';
            else if (c == '/')
                c = '_';
        }
        while (!out.empty() && out.back() == '=')
            out.pop_back();
    }
    return out;
}
std::string base64_encode(std::string_view bytes, bool url) {
    return base64_encode({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, url);
}
std::vector<std::uint8_t> base64_decode(std::string_view value, bool url) {
    std::string text(value);
    if (url) {
        for (auto& c : text) {
            if (c == '-')
                c = '+';
            else if (c == '_')
                c = '/';
        }
        if (text.size() % 4 == 1)
            throw Error("Invalid base64 length");
        while (text.size() % 4)
            text += '=';
    }
    if (text.size() % 4 || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw Error("Invalid base64 length");
    std::size_t padding = 0;
    if (!text.empty() && text.back() == '=')
        ++padding;
    if (text.size() > 1 && text[text.size() - 2] == '=')
        ++padding;
    for (std::size_t i = 0; i < text.size() - padding; ++i) {
        const auto c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
              c == '/'))
            throw Error("Invalid base64 character");
    }
    std::vector<std::uint8_t> out(text.size() / 4 * 3);
    if (text.empty())
        return out;
    const auto n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(text.data()),
                                   static_cast<int>(text.size()));
    if (n < 0)
        throw Error("Invalid base64 content");
    out.resize(static_cast<std::size_t>(n) - padding);
    if (base64_encode(out, false) != text)
        throw Error("Invalid non-canonical base64 content");
    return out;
}
bool constant_time_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
fs::path executable_path() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n == path.size())
        throw Error("Cannot resolve executable path");
    path.resize(n);
    return fs::path(path);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size))
        throw Error("Cannot resolve executable path");
    return fs::weakly_canonical(path_from_utf8(path.c_str()));
#else
    std::array<char, 65536> path{};
    const auto n = readlink("/proc/self/exe", path.data(), path.size());
    if (n < 0)
        throw Error("Cannot resolve executable path");
    return path_from_utf8({path.data(), static_cast<std::size_t>(n)});
#endif
}
std::uint32_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}
std::string read_file(const fs::path& path, std::size_t limit) {
    return read_file_range(path, 0, limit);
}
std::string read_file_range(const fs::path& path, std::uint64_t offset, std::size_t limit) {
    if (offset > static_cast<std::uint64_t>(INT64_MAX))
        throw Error("File offset exceeds supported range");
#ifdef _WIN32
    NativeHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "open " + path_text(path));
    if (offset) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    std::string out;
    std::array<char, 65536> buffer{};
    while (out.size() < limit) {
        const auto wanted = static_cast<DWORD>(std::min(buffer.size(), limit - out.size()));
        DWORD count = 0;
        if (!ReadFile(file.get(), buffer.data(), wanted, &count, nullptr))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "read " + path_text(path));
        if (!count)
            break;
        out.append(buffer.data(), count);
    }
    return out;
#else
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::system_error(errno, std::generic_category(), "open " + path_text(path));
    if (offset) {
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file)
            throw Error("Cannot seek " + path_text(path));
    }
    std::string out;
    std::array<char, 65536> buffer{};
    while (file && out.size() < limit) {
        const auto n = std::min(buffer.size(), limit - out.size());
        file.read(buffer.data(), static_cast<std::streamsize>(n));
        out.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
    }
    if (file.bad())
        throw Error("Cannot read " + path_text(path));
    return out;
#endif
}
Json read_json(const fs::path& path, std::size_t limit) {
    const auto value = read_file(path, limit + 1);
    if (value.size() > limit)
        throw Error("JSON file exceeds its size limit: " + path_text(path));
    return Json::parse(value);
}
std::optional<Json> read_json_optional(const fs::path& path, std::size_t limit) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (ec)
            throw std::system_error(ec);
        return std::nullopt;
    }
    return read_json(path, limit);
}
void write_file(const fs::path& path, std::string_view bytes, bool append) {
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!file)
        throw Error("Cannot open " + path_text(path));
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file)
        throw Error("Cannot write " + path_text(path));
}
void write_json_atomic(const fs::path& path, const Json& value) {
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    const auto temporary = path_from_utf8(path_text(path) + "." + uuid() + ".tmp");
    try {
        write_file(temporary, value.dump(2));
#ifdef _WIN32
        const auto flush_handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (flush_handle == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "open JSON state for flush");
        const bool flushed = FlushFileBuffers(flush_handle) != FALSE;
        const auto flush_error = GetLastError();
        CloseHandle(flush_handle);
        if (!flushed)
            throw std::system_error(static_cast<int>(flush_error), std::system_category(),
                                    "flush JSON state");
        replace_state_file(temporary, path);
#else
        const int fd = open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category());
        const int synced = fsync(fd);
        close(fd);
        if (synced)
            throw std::system_error(errno, std::generic_category(), "flush JSON state");
        fs::rename(temporary, path);
        const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            fsync(directory);
            close(directory);
        }
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(temporary, ec);
        throw;
    }
}
std::string json_string(const Json& object, std::string_view key, std::string fallback) {
    const auto it = object.find(std::string(key));
    return it == object.end() || it->is_null() ? fallback : it->get<std::string>();
}
bool json_bool(const Json& object, std::string_view key, bool fallback) {
    const auto it = object.find(std::string(key));
    return it == object.end() || it->is_null() ? fallback : it->get<bool>();
}
std::uint64_t json_uint(const Json& object, std::string_view key, std::uint64_t fallback) {
    const auto it = object.find(std::string(key));
    if (it == object.end() || it->is_null())
        return fallback;
    if (!it->is_number_integer() || it->get<double>() < 0)
        throw Error(std::string(key) + " must be a nonnegative integer");
    return it->get<std::uint64_t>();
}
double json_number(const Json& object, std::string_view key, double fallback) {
    const auto it = object.find(std::string(key));
    return it == object.end() || it->is_null() ? fallback : it->get<double>();
}
std::vector<std::string> json_strings(const Json& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    return it == object.end() || it->is_null() ? std::vector<std::string>{}
                                               : it->get<std::vector<std::string>>();
}
Json canonical_json(const Json& value) {
    if (value.is_array()) {
        Json out = Json::array();
        for (const auto& child : value)
            out.push_back(canonical_json(child));
        return out;
    }
    if (!value.is_object())
        return value;
    std::map<std::string, Json> ordered;
    for (auto it = value.begin(); it != value.end(); ++it)
        ordered.emplace(it.key(), canonical_json(it.value()));
    Json out = Json::object();
    for (const auto& [key, child] : ordered)
        out[key] = child;
    return out;
}
std::string url_encode(std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const auto raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}
std::string url_decode(std::string_view value, bool plus_space) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() && nibble(value[i + 1]) >= 0 &&
            nibble(value[i + 2]) >= 0) {
            out += static_cast<char>((nibble(value[i + 1]) << 4) | nibble(value[i + 2]));
            i += 2;
        } else
            out += (value[i] == '+' && plus_space) ? ' ' : value[i];
    }
    return sanitize_utf8(out);
}
Json query_parameters(std::string_view query) {
    Json result = Json::object();
    for (const auto& pair : split(query, '&', false)) {
        const auto p = pair.find('=');
        result[url_decode(std::string_view(pair).substr(0, p))] =
            p == pair.npos ? "" : url_decode(std::string_view(pair).substr(p + 1));
    }
    return result;
}
Url Url::parse(std::string_view value) {
    Url result;
    auto text = trim(value);
    const auto colon = text.find(':');
    if (colon == text.npos || colon == 0)
        throw Error("URL must be absolute");
    result.scheme = lower(text.substr(0, colon));
    std::string_view rest(text);
    rest.remove_prefix(colon + 1);
    if (rest.starts_with("//")) {
        rest.remove_prefix(2);
        const auto end = rest.find_first_of("/?#");
        auto authority = rest.substr(0, end);
        rest = end == rest.npos ? std::string_view{} : rest.substr(end);
        const auto at = authority.rfind('@');
        if (at != authority.npos) {
            result.userinfo = std::string(authority.substr(0, at));
            authority.remove_prefix(at + 1);
        }
        if (authority.starts_with('[')) {
            const auto close = authority.find(']');
            if (close == authority.npos)
                throw Error("Invalid IPv6 URL");
            result.host = lower(std::string(authority.substr(1, close - 1)));
            if (close + 1 < authority.size()) {
                if (authority[close + 1] != ':')
                    throw Error("Invalid URL authority");
                result.port = std::string(authority.substr(close + 2));
            }
        } else {
            const auto port = authority.rfind(':');
            result.host = lower(std::string(authority.substr(0, port)));
            if (port != authority.npos)
                result.port = std::string(authority.substr(port + 1));
        }
        if (result.host.empty())
            throw Error("URL host is empty");
    }
    const auto hash = rest.find('#');
    if (hash != rest.npos) {
        result.fragment = std::string(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    const auto q = rest.find('?');
    result.path = std::string(rest.substr(0, q));
    if (q != rest.npos)
        result.query = std::string(rest.substr(q + 1));
    if (result.path.empty() && !result.host.empty())
        result.path = "/";
    if (!result.port.empty()) {
        const auto p = digits(result.port);
        if (p < 1 || p > 65535)
            throw Error("Invalid URL port");
        if ((result.scheme == "http" && p == 80) || (result.scheme == "https" && p == 443))
            result.port.clear();
    }
    if ((result.scheme == "http" || result.scheme == "https") && result.host.empty())
        throw Error("URL host is required");
    return result;
}
std::string Url::origin() const {
    if (host.empty())
        return scheme + ":";
    return scheme + "://" + (host.find(':') != host.npos ? "[" + host + "]" : host) +
           (port.empty() ? "" : ":" + port);
}
std::string Url::str() const {
    return origin() + path + (query.empty() ? "" : "?" + query) + (fragment.empty() ? "" : "#" + fragment);
}

HttpResult http_request(std::string_view method, std::string_view url, std::string_view body,
                        const Json& headers, Millis timeout, std::size_t max_bytes, const Cancel& cancel) {
    static const bool initialized = []() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw Error("HTTP initialization failed");
        return true;
    }();
    (void)initialized;
    struct State {
        HttpResult result;
        std::size_t max_bytes;
        Cancel cancel;
        bool oversized = false;
    };
    State state{{}, max_bytes, cancel};
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
        throw Error("HTTP allocation failed");
    const auto uri = std::string(url), verb = std::string(method);
    curl_slist* raw_headers = nullptr;
    for (auto it = headers.begin(); it != headers.end(); ++it)
        raw_headers =
            curl_slist_append(raw_headers, (it.key() + ": " + it.value().get<std::string>()).c_str());
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(raw_headers,
                                                                                  curl_slist_free_all);
    curl_easy_setopt(handle.get(), CURLOPT_URL, uri.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_CUSTOMREQUEST, verb.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, raw_headers);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS,
                     static_cast<long>(std::clamp<std::int64_t>(timeout.count(), 1, 2147483647)));
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(std::min<std::int64_t>(5000, timeout.count())));
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!body.empty() || verb == "POST" || verb == "PUT") {
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(
        handle.get(), CURLOPT_WRITEFUNCTION,
        +[](char* data, std::size_t a, std::size_t b, void* context) -> std::size_t {
            auto& s = *static_cast<State*>(context);
            if (b && a > std::numeric_limits<std::size_t>::max() / b)
                return 0;
            const auto size = a * b;
            if (size > s.max_bytes - s.result.body.size()) {
                s.oversized = true;
                return 0;
            }
            s.result.body.append(data, size);
            return size;
        });
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(
        handle.get(), CURLOPT_HEADERFUNCTION,
        +[](char* data, std::size_t a, std::size_t b, void* context) -> std::size_t {
            auto& s = *static_cast<State*>(context);
            const auto size = a * b;
            const std::string_view line(data, size);
            const auto p = line.find(':');
            if (p != line.npos)
                s.result.headers[lower(trim(line.substr(0, p)))] = trim(line.substr(p + 1));
            return size;
        });
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        handle.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            const auto& s = *static_cast<State*>(context);
            return s.cancel && s.cancel->cancelled() ? 1 : 0;
        });
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &state);
    const auto code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) {
        if (state.oversized)
            throw Error("HTTP response exceeds its byte limit");
        if (cancel && cancel->cancelled())
            throw Cancelled();
        throw Error(std::string("HTTP request failed: ") + curl_easy_strerror(code));
    }
    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    state.result.status = static_cast<int>(status);
    return state.result;
}
} // namespace devbox
