#include "devbox/research.hpp"
#include "devbox/web_retailers.hpp"
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 65536) return 0;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    for (const auto* type : {"text/html; charset=utf-8", "application/json"}) {
        try {
            devbox::web::Transfer response;
            response.url = "https://www.amazon.co.uk/dp/B012345678";
            response.status = 200;
            response.headers = devbox::Json{{"content-type", type}};
            response.body = bytes;
            const auto document = devbox::web::extract_document(response);
            (void)devbox::web::offer_view(document, 0, 16).dump();
        } catch (const devbox::Error&) {
        } catch (const devbox::Json::exception&) {
        }
    }
    try { (void)devbox::web::retry_after_millis(bytes); } catch (const devbox::Error&) {}
    return 0;
}
