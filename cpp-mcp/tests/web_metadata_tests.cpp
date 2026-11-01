#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
int main() {
    try {
        for (const auto* kind : {"Product", "Article", "WebPage", "BlogPosting"}) {
            for (const bool separate_scripts : {false, true}) {
                Json input = Json::array(), expected = Json::array();
                for (unsigned i = 0; i < 8; ++i) {
                    Json item{{"@type", kind},
                              {"name", std::string(2040, 'n') + "\"\xe6\x89\x8b"},
                              {"description", std::string(2048, 'd')}};
                    input.push_back(item);
                    item["schema_path"] = separate_scripts ? "$" : "$[" + std::to_string(i) + "]";
                    if (std::string_view(kind) != "Product")
                        item["attributed_to_document"] = separate_scripts;
                    expected.push_back(std::move(item));
                }
                // Choose an input whose independently serialized expected metadata
                // exactly fills the public-product metadata budget, including commas.
                const auto excess = expected.dump().size() - 32768;
                require(excess > 0 && excess < 2048, "Boundary fixture fits the per-field clipping policy");
                input[7]["description"] = std::string(2048 - excess, 'd');
                expected[7]["description"] = input[7]["description"];
                require(expected.dump().size() == 32768, "Independent exact metadata boundary");
                web::Transfer response;
                response.url = response.final_url = "https://fixture.example/products";
                response.status = 200;
                response.headers["content-type"] = separate_scripts ? "text/html" : "application/json";
                const auto set_body = [&] {
                    if (!separate_scripts) {
                        response.body = input.dump();
                        return;
                    }
                    response.body = "<html><head>";
                    for (const auto& item : input)
                        response.body += "<script type='application/ld+json'>" + item.dump() + "</script>";
                    response.body +=
                        "</head><body><main><p>Product catalogue fixture.</p></main></body></html>";
                };
                set_body();
                auto result = web::extract_document(response);
                require(result["structured_metadata"] == expected &&
                            result["structured_metadata_truncated"] == false,
                        "Exactly fitting metadata must retain every record and its evidence path");
                input[7]["description"].get_ref<std::string&>() += 'x';
                set_body();
                result = web::extract_document(response);
                expected.erase(expected.size() - 1);
                require(result["structured_metadata"] == expected &&
                            result["structured_metadata"].dump().size() <= 32768 &&
                            result["structured_metadata_truncated"] == true,
                        "Metadata exceeding the budget by one separator byte must be rejected");
                input.push_back(Json{{"@type", kind}, {"name", "Small later item"}});
                set_body();
                result = web::extract_document(response);
                require(result["structured_metadata"].size() == 8 &&
                            result["structured_metadata"].back()["name"] == "Small later item" &&
                            result["structured_metadata_truncated"] == true,
                        "An oversized item must not prevent later smaller evidence from fitting");
            }
        }
        std::cout
            << "Metadata exact boundary, overrun, multiple scripts, escapes and later-fit cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
