#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace devbox {
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;
constexpr std::uint64_t max_safe_integer = 9007199254740991ULL;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Cancelled : Error {
    Cancelled() : Error("Command cancelled by the MCP client.") {}
};
class Cancellation {
    std::atomic_bool cancelled_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;

  public:
    void cancel() noexcept;
    bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }
    void check() const {
        if (cancelled())
            throw Cancelled();
    }
    bool wait_for(Millis duration);
};
using Cancel = std::shared_ptr<Cancellation>;
std::string trim(std::string_view value);
std::string lower(std::string value);
bool starts_with(std::string_view value, std::string_view prefix);
std::vector<std::string> split(std::string_view value, char separator, bool keep_empty = true);
std::string join(const std::vector<std::string>& values, std::string_view separator);
std::string replace_all(std::string value, std::string_view from, std::string_view to);
std::optional<std::string> environment(std::string_view name);
void set_environment(std::string_view name, const std::optional<std::string>& value);
std::string env_or(std::string_view name, std::string_view fallback);
bool env_bool(std::string_view name, bool fallback);
std::uint64_t env_uint(std::string_view name, std::uint64_t fallback);
std::uint64_t unix_millis();
std::string utc_now();
std::string utc_from_millis(std::int64_t milliseconds);
std::optional<std::int64_t> parse_utc(std::string_view value);
std::string uuid();
std::vector<std::uint8_t> random_bytes(std::size_t size);
std::string hex(std::span<const std::uint8_t> bytes);
std::string sha256(std::span<const std::uint8_t> bytes);
std::string sha256(std::string_view bytes);
std::string sha256_file(const fs::path& path, std::optional<std::uint64_t> limit = std::nullopt);
std::string base64_encode(std::span<const std::uint8_t> bytes, bool url = false);
std::string base64_encode(std::string_view bytes, bool url = false);
std::vector<std::uint8_t> base64_decode(std::string_view value, bool url = false);
bool constant_time_equal(std::string_view a, std::string_view b);
std::u16string to_utf16(std::string_view value);
std::string from_utf16(std::u16string_view value);
std::string sanitize_utf8(std::string_view value);
std::size_t js_length(std::string_view value);
std::string js_slice(std::string_view value, std::size_t start, std::size_t end);
fs::path path_from_utf8(std::string_view value);
std::string path_text(const fs::path& value);
fs::path executable_path();
std::uint32_t process_id();
std::string read_file(const fs::path& path, std::size_t limit = 64 * 1024 * 1024);
std::string read_file_range(const fs::path& path, std::uint64_t offset, std::size_t limit);
Json read_json(const fs::path& path, std::size_t limit = 16 * 1024 * 1024);
std::optional<Json> read_json_optional(const fs::path& path, std::size_t limit = 16 * 1024 * 1024);
void write_file(const fs::path& path, std::string_view bytes, bool append = false);
void write_json_atomic(const fs::path& path, const Json& value);
void replace_state_file(const fs::path& source, const fs::path& target);
std::string json_string(const Json& object, std::string_view key, std::string fallback = {});
bool json_bool(const Json& object, std::string_view key, bool fallback = false);
std::uint64_t json_uint(const Json& object, std::string_view key, std::uint64_t fallback = 0);
double json_number(const Json& object, std::string_view key, double fallback = 0);
std::vector<std::string> json_strings(const Json& object, std::string_view key);
Json canonical_json(const Json& value);
std::string url_encode(std::string_view value);
std::string url_decode(std::string_view value, bool plus_space = true);
Json query_parameters(std::string_view query);
struct Url {
    std::string scheme, host, port, path, query, fragment, userinfo;
    static Url parse(std::string_view value);
    std::string origin() const;
    std::string str() const;
};
struct HttpResult {
    int status = 0;
    std::string body;
    Json headers = Json::object();
};
HttpResult http_request(std::string_view method, std::string_view url, std::string_view body = {},
                        const Json& headers = Json::object(), Millis timeout = Millis(10000),
                        std::size_t max_bytes = 256 * 1024, const Cancel& cancel = {});
} // namespace devbox
