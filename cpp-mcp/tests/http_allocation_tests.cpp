#include "devbox/common.hpp"
#include "devbox/native.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <iostream>
#include <string_view>

using namespace devbox;
namespace {
// A standalone executable installs libcurl's documented allocator hooks before
// its first initialization. This counts C allocations, which operator new's
// telemetry cannot see. Fault injection is limited to our exact header value.
std::atomic<std::int64_t> outstanding{0};
std::atomic<bool> fail_header{false};
std::atomic<unsigned> injected{0};
constexpr auto failing_header = "Z-Failure-Probe: allocation-test";

void* tracked_malloc(std::size_t bytes) {
    void* pointer = std::malloc(bytes);
    if (pointer)
        ++outstanding;
    return pointer;
}
void tracked_free(void* pointer) {
    if (pointer) {
        --outstanding;
        std::free(pointer);
    }
}
void* tracked_realloc(void* pointer, std::size_t bytes) {
    if (!pointer)
        return tracked_malloc(bytes);
    if (!bytes) {
        tracked_free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, bytes);
}
void* tracked_calloc(std::size_t count, std::size_t bytes) {
    void* pointer = std::calloc(count, bytes);
    if (pointer)
        ++outstanding;
    return pointer;
}
char* tracked_strdup(const char* text) {
    if (std::strcmp(text, failing_header) == 0 && fail_header.exchange(false)) {
        ++injected;
        return nullptr;
    }
    const auto bytes = std::strlen(text) + 1;
    auto* copy = static_cast<char*>(tracked_malloc(bytes));
    if (copy)
        std::memcpy(copy, text, bytes);
    return copy;
}
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
void request(const Json& headers) {
    // No network, DNS, server, credentials or external endpoint is involved.
    (void)http_request("GET", "devbox-test-invalid://allocation-fixture", "", headers, Millis(100), 16, {},
                       true);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected conversion or allocation case");
        const std::string_view mode(argv[1]);
        require(mode == "conversion" || mode == "allocation", "Unknown allocation case");
        require(curl_global_init_mem(CURL_GLOBAL_DEFAULT, tracked_malloc, tracked_free, tracked_realloc,
                                     tracked_strdup, tracked_calloc) == CURLE_OK,
                "Could not install isolated libcurl allocation hooks");
        // http_request takes its own global-init reference on the first call.
        ScopeExit cleanup([] {
            curl_global_cleanup();
            curl_global_cleanup();
        });
        for (unsigned warmup = 0; warmup < 3; ++warmup) {
            try {
                request(Json::object());
                throw Error("The fixture protocol must be rejected");
            } catch (const Error& error) {
                require(std::string_view(error.what()).starts_with("HTTP request failed:"),
                        "Warmup failed before the expected protocol rejection");
            }
        }
        const auto baseline = outstanding.load();
        for (unsigned repeat = 0; repeat < 32; ++repeat) {
            bool rejected = false;
            if (mode == "conversion") {
                try {
                    request(Json{{"A-First", "retained-header"}, {"Z-Bad", 7}});
                } catch (const Json::type_error&) {
                    rejected = true;
                }
            } else {
                fail_header = true;
                try {
                    request(Json{{"A-First", "retained-header"}, {"Z-Failure-Probe", "allocation-test"}});
                } catch (const Error& error) {
                    require(std::string_view(error.what()) == "HTTP header allocation failed",
                            "Header allocation failure was not reported before transfer");
                    rejected = true;
                }
                require(!fail_header && injected == repeat + 1,
                        "The requested header-allocation failure was not injected");
            }
            require(rejected, "Header failure was not reported");
            const auto retained = outstanding.load() - baseline;
            if (retained) {
                std::cerr << "case=" << mode << " repeat=" << repeat
                          << " retained_libcurl_allocations=" << retained << '\n';
            }
            require(retained == 0, "Failed HTTP header construction leaked libcurl allocations");
        }
        try {
            request(Json{{"A-First", "retained-header"}, {"Z-Failure-Probe", "allocation-test"}});
            throw Error("The fixture protocol must be rejected");
        } catch (const Error& error) {
            require(std::string_view(error.what()).starts_with("HTTP request failed:"),
                    "Normal header construction did not reach protocol validation");
        }
        require(outstanding == baseline, "Completed header construction leaked allocations");
        std::cout << "case=" << mode << " repetitions=32 retained_libcurl_allocations=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
