#include "devbox/fast_deflate.hpp"
#include "devbox/research.hpp"
#include "devbox/web_retailers.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <zlib.h>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 65536)
        return 0;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    // Differential compression checks use an independent decoder on every
    // mutated payload, covering native bit output alongside the text parsers.
    std::vector<char> encoded(size + size / 8 + 66);
    const auto count = devbox::native_deflate(
        bytes, encoded, size && (data[0] & 1) ? devbox::DeflateWrapper::zlib : devbox::DeflateWrapper::gzip);
    if (!count)
        std::abort();
    std::vector<char> decoded(size + 1);
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 32) != Z_OK)
        std::abort();
    stream.next_in = reinterpret_cast<Bytef*>(encoded.data());
    stream.avail_in = static_cast<uInt>(count);
    stream.next_out = reinterpret_cast<Bytef*>(decoded.data());
    stream.avail_out = static_cast<uInt>(decoded.size());
    const bool equal = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == size &&
                       stream.avail_in == 0 && std::string_view(decoded.data(), size) == bytes;
    inflateEnd(&stream);
    if (!equal)
        std::abort();
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
    try {
        (void)devbox::web::retry_after_millis(bytes);
    } catch (const devbox::Error&) {
    }
    return 0;
}
