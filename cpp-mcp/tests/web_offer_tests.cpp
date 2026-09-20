#include "devbox/research.hpp"
#include <iostream>

using namespace devbox;
void require(bool condition, const char* message) {
    if (!condition)
        throw Error(message);
}
Json extract(std::string html) {
    web::Transfer response;
    response.url = "https://shop.example/products/phone";
    response.status = 200;
    response.completed_at = "2026-09-20T18:00:00Z";
    response.body =
        "<html><title>Phone</title><main><p>" + std::string(140, 'x') + "</p>" + html + "</main></html>";
    response.headers = Json{{"content-type", "text/html"}};
    return web::extract_document(response);
}
std::string ld(const Json& json) {
    return "<script type='application/ld+json'>" + json.dump() + "</script>";
}
Json product() {
    return Json{{"@type", "Product"},
                {"name", "Phone Exact 256GB"},
                {"sku", "P256"},
                {"offers", Json{{"@type", "Offer"},
                                {"price", "559.00"},
                                {"priceCurrency", "GBP"},
                                {"availability", "https://schema.org/InStock"},
                                {"url", "/products/phone?variant=1001"}}}};
}
int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            // Local replay only: retain the real public URL for exact variant joins.
            web::Transfer response;
            response.url = argv[2];
            response.status = 200;
            response.body = read_file(argv[1], 2 * 1024 * 1024);
            response.headers = Json{{"content-type", "text/html; charset=utf-8"}};
            const auto result = web::extract_document(response);
            std::cout << web::offer_view(result, 0, 64).dump(2) << '\n';
            return 0;
        }
        auto p = product();
        const auto exact = web::offer_view(extract(ld(p)));
        require(exact["offers"][0]["price"] == "559.00", "exact decimal price");
        require(exact["offers"][0]["sku"] == "P256", "product SKU retained");
        require(exact["checked_at"] == "2026-09-20T18:00:00Z", "check timestamp");
        require(exact["offers"][0]["missing_fields"] == Json::array({"itemCondition"}),
                "unknown condition stays unknown");

        auto bank = p["offers"];
        bank["price"] = 536;
        bank["url"] = "/products/phone?variant=1002";
        p["offers"] = Json::array({p["offers"], bank});
        const std::string variants = R"(<variant-selects data-url='/products/phone'>
          <select name='options[Color]'><option>Black</option></select>
          <select name='options[Size]'><option>12GB/256GB</option></select>
          <select name='options[Paying by Bank Transfer?]'><option>No</option></select>
          <script type='application/json'>[
          {"id":1001,"title":"Black / 12GB/256GB / No","options":["Black","12GB/256GB","No"],"available":true},
          {"id":1002,"title":"Black / 12GB/256GB / Yes","options":["Black","12GB/256GB","Yes"],"available":true}
          ]</script></variant-selects><span class='hidden'>Sold out</span>)";
        auto result = web::offer_view(extract(ld(p) + variants));
        require(result["offers"].size() == 2, "separate payment offers");
        require(result["offers"][1]["option_names"][2] == "Paying by Bank Transfer?",
                "payment label preserved");
        require(result["offers"][1]["options"][2] == "Yes", "bank transfer condition preserved");
        require(result["offers"][0]["stock_status_reported"] == "InStock",
                "hidden badge not stock authority");
        auto doc = extract(ld(p) + variants);
        require(web::offer_view(doc, 0, 1)["next_offer_offset"] == 1, "offer pagination");
        require(web::offer_view(doc, 1, 1)["offers"][0]["price"] == "536.00", "offer page identity");
        require(!web::offer_view(doc, 2, 1).contains("next_offer_offset"), "terminal page");

        auto conflict_variants = variants;
        conflict_variants.replace(conflict_variants.find("\"available\":true"), 16, "\"available\":false");
        result = web::offer_view(extract(ld(p) + conflict_variants));
        require(result["offers"][0]["assessment"] == "conflicting_source_data", "stock conflict explicit");
        auto unrelated = variants;
        unrelated.replace(unrelated.find("data-url='/products/phone'"), 26, "data-url='/products/other'");
        result = web::offer_view(extract(ld(p) + unrelated));
        require(!result["offers"][0].contains("variant_name"), "no cross-product variant join");

        p = product();
        p["offers"]["priceValidUntil"] = "2026-09-19";
        p["offers"]["availability"] = "https://schema.org/PreOrder";
        p["offers"]["itemCondition"] = "https://schema.org/RefurbishedCondition";
        p["offers"]["priceCurrency"] = "USD";
        result = web::offer_view(extract(ld(p)));
        require(result["offers"][0]["currency"] == "USD", "currency not inferred from UK query");
        require(result["offers"][0]["qualifiers"][0] == "priceValidUntil_expired", "expired price explicit");
        require(result["offers"][0]["stock_status_reported"] == "PreOrder", "preorder distinct from stock");
        require(result["offers"][0]["itemCondition"] == "RefurbishedCondition", "condition retained");
        p["offers"] = Json{
            {"@type", "AggregateOffer"}, {"lowPrice", 399}, {"highPrice", 999}, {"priceCurrency", "GBP"}};
        result = web::offer_view(extract(ld(p)));
        require(result["offers"][0]["assessment"] == "aggregate_only" && result["offers"][0]["price"] == "",
                "from price must not become a purchasable variant");

        p = product();
        p["@id"] = "#phone";
        auto offer = p["offers"];
        offer["@id"] = "#offer";
        p["offers"] = Json{{"@id", "#offer"}};
        Json graph{{"@graph", Json::array({p, offer})}};
        result = web::offer_view(extract(ld(graph)));
        require(result["offers"][0]["product_name"] == "Phone Exact 256GB", "graph offer resolved");
        graph["@graph"].push_back(Json{{"@id", "#offer"}, {"@type", "Thing"}});
        result = web::offer_view(extract(ld(graph)));
        for (const auto& row : result["offers"])
            require(row["product_name"] == "", "ambiguous reference never assigned to product");

        auto group = Json{{"@type", "ProductGroup"},
                          {"name", "Phone Family"},
                          {"sku", "GROUP-NOT-SKU"},
                          {"hasVariant", Json::array({product()})}};
        result = web::offer_view(extract(ld(group)));
        require(result["offers"][0]["sku"] == "P256", "variant not group SKU");
        p = product();
        p["offers"]["price"] = "£559 or £18 monthly";
        result = web::offer_view(extract(ld(p)));
        require(result["offers"][0]["price"] == "", "ambiguous text not parsed as exact price");
        p = product();
        p["offers"]["priceSpecification"] =
            Json{{"price", 18}, {"priceCurrency", "GBP"}, {"billingDuration", "P30M"}};
        result = web::offer_view(extract(ld(p)));
        require(result["offers"][0]["assessment"] == "conflicting_source_data",
                "monthly versus cash conflict");

        auto map =
            R"(<script type='application/json' data-element='variants-data'>{"1001":{"options":["12+256GB","White"]}}</script>)";
        result = web::offer_view(extract(ld(product()) + map));
        require(result["offers"][0]["variant_name"] == "12+256GB / White", "explicit variant-ID map");
        auto source_currency = web::extract_offer_records(Json::array({product()}), Json::array(),
                                                          "https://shop.example/products/phone?currency=GBP");
        require(source_currency["records"][0]["url"] ==
                    "https://shop.example/products/phone?variant=1001&currency=GBP",
                "variant link retains currency");
        p = product();
        p["offers"]["priceCurrency"] = "USD";
        source_currency = web::extract_offer_records(Json::array({p}), Json::array(),
                                                     "https://shop.example/products/phone?currency=GBP");
        require(!source_currency["records"][0].contains("currency_context_preserved"),
                "no currency conversion inferred");
        std::cout << "native product offer regressions passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
