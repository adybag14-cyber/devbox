#include "devbox/allocator.hpp"
#include "devbox/research.hpp"
#include "devbox/utf8.hpp"
#include <iostream>
#include <random>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
bool reference(std::string_view input) {
    try {
        (void)Json(std::string(input)).dump();
        return true;
    } catch (const Json::type_error&) {
        return false;
    }
}
std::string encoded(std::uint32_t point) {
    if (point <= 0xffff)
        return from_utf16(std::u16string(1, static_cast<char16_t>(point)));
    point -= 0x10000;
    return from_utf16(std::u16string{static_cast<char16_t>(0xd800 + point / 1024),
                                     static_cast<char16_t>(0xdc00 + point % 1024)});
}
int main() {
    try {
        static_assert(valid_utf8("ASCII"));
        static_assert(!valid_utf8("\xc0\x80"));
        std::string unicode;
        unicode.reserve(5 * 1024 * 1024);
        std::size_t scalars = 0;
        for (std::uint32_t point = 0; point <= 0x10ffff; ++point) {
            if (point >= 0xd800 && point <= 0xdfff)
                continue;
            const auto bytes = encoded(point);
            require(valid_utf8(bytes),
                    "Every Unicode scalar must be accepted, including NUL and noncharacters");
            unicode += bytes;
            ++scalars;
        }
        require(reference(unicode), "The retained JSON policy accepts all encoded scalar values");
        const auto before = allocator_counters();
        const bool valid = valid_utf8(unicode);
        const auto after = allocator_counters();
        require(valid && before.calls == after.calls && before.allocated == after.allocated,
                "Full UTF-8 validation must allocate no temporary buffers");
        std::size_t differential = 0;
        for (unsigned a = 0; a < 256; ++a) {
            const std::string one(1, static_cast<char>(a));
            require(valid_utf8(one) == reference(one), "Single-byte validation differs from existing policy");
            ++differential;
            for (unsigned b = 0; b < 256; ++b) {
                const std::string pair{static_cast<char>(a), static_cast<char>(b)};
                require(valid_utf8(pair) == reference(pair),
                        "Two-byte validation differs from existing policy");
                ++differential;
            }
        }
        std::mt19937 random(0x20261031);
        for (unsigned trial = 0; trial < 20000; ++trial) {
            std::string input;
            for (unsigned n = 1 + random() % 24; n > 0; --n)
                input += static_cast<char>(random() & 255);
            require(valid_utf8(input) == reference(input), "Random bytes differ from retained UTF-8 policy");
            ++differential;
        }
        for (const std::string input :
             {"\xe0\x9f\xbf", "\xed\xa0\x80", "\xef\xbf", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
              "\xf5\x80\x80\x80", "\xf0\x90\x80", "\xf0!\x80\x80"})
            require(!valid_utf8(input) && !reference(input), "Malformed scalar boundary was accepted");
        web::Transfer page;
        page.url = page.final_url = "https://fixture.example/phone";
        page.status = 200;
        page.headers["content-type"] = "text/plain; charset=utf-8";
        page.body = std::string(131072, 'a') + "\xf0\x9f\x93\xb1";
        auto result = web::extract_document(page);
        require(result["status"] == "ok" && result["content_truncated"] == true &&
                    result["body_sha256"] == sha256(page.body),
                "Large valid input keeps original body hash and bounded evidence");
        page.body += '\xff';
        result = web::extract_document(page);
        require(result["status"] == "unsupported_encoding" && result["text"] == "",
                "Invalid input beyond the visible excerpt must still be rejected");
        page.body = "Phone \xa3 123.45 \x80";
        page.headers["content-type"] = "text/plain; charset=windows-1252";
        result = web::extract_document(page);
        require(result["text"] == "Phone \xc2\xa3 123.45 \xe2\x82\xac" &&
                    result["body_sha256"] == sha256(page.body),
                "Legacy conversion and original-byte provenance must stay intact");
        page.headers["content-type"] = "text/plain; charset=shift-jis";
        require(web::extract_document(page)["status"] == "unsupported_encoding",
                "Unsupported declared encodings remain rejected");
        std::cout << Json{{"ok", true},
                          {"valid_scalars", scalars},
                          {"differential_cases", differential},
                          {"validation_allocations", after.calls - before.calls}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
