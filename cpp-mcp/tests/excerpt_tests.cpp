#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
int main() {
    try {
        const std::string text = "\xce\xb1\xce\xb2\xf0\x9f\x93\xb1"
                                 "ABCDEF";
        const std::string storage = "XX" + text + "YY";
        const auto view = std::string_view(storage).substr(2, text.size());
        struct Case {
            std::string query, expected, selection;
            std::size_t limit, begin, end, byte_begin, byte_end;
        };
        for (const auto& test : std::vector<Case>{
                 {"C", "BCDE", "query_match", 4, 4, 8, 9, 13},
                 {"\xf0\x9f\x93\xb1",
                  "\xce\xb2\xf0\x9f\x93\xb1"
                  "AB",
                  "query_match", 4, 1, 5, 2, 10},
                 {"F", "EF", "query_match", 2, 7, 9, 12, 14},
                 {"missing",
                  "\xce\xb1\xce\xb2\xf0\x9f\x93\xb1"
                  "A",
                  "lead", 4, 0, 4, 0, 9},
                 {"missing DEF", "CDEF", "query_match", 4, 5, 9, 10, 14},
                 {"C", "", "query_match", 0, 5, 5, 10, 10},
                 {"F", text, "full_text", 50, 0, 9, 0, 14},
             }) {
            const auto result = web::evidence_excerpt_details(view, test.query, test.limit);
            require(result["excerpt"] == test.expected && result["excerpt_selection"] == test.selection,
                    "Excerpt content and match selection preserve the established policy");
            require(result["excerpt_start_char"] == test.begin && result["excerpt_end_char"] == test.end &&
                        result["excerpt_start_byte"] == test.byte_begin &&
                        result["excerpt_end_byte"] == test.byte_end &&
                        result["excerpt_chars"] == test.end - test.begin &&
                        result["excerpt_bytes"] == test.byte_end - test.byte_begin &&
                        result["excerpt_truncated"] == (test.begin != 0 || test.end != 9),
                    "Offsets refer to original UTF-8 bytes and Unicode scalar positions");
        }
        const auto folded = web::evidence_excerpt_details("A\xe2\x84\xaa"
                                                          "B",
                                                          "k", 1);
        require(folded["excerpt"] == "\xe2\x84\xaa" && folded["excerpt_start_char"] == 1 &&
                    folded["excerpt_start_byte"] == 1 && folded["excerpt_end_byte"] == 4,
                "Case folding must not change offsets into the original bytes");
        const auto empty = web::evidence_excerpt_details({}, "anything", 0);
        require(empty["excerpt"] == "" && empty["excerpt_bytes"] == 0 && empty["excerpt_start_byte"] == 0 &&
                    empty["excerpt_selection"] == "full_text" && empty["excerpt_truncated"] == false,
                "Empty input has a complete empty excerpt");
        std::cout << "Unicode, query, empty and bounded-view excerpt offsets passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
