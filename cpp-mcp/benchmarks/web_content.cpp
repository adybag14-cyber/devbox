#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "Complete article extraction with repeated DOM attributes and links"}}
                         .dump()
                  << '\n';
        for (const bool decorated : {false, true}) {
            for (const unsigned paragraphs : {16U, 128U, 512U}) {
                web::Transfer response;
                response.url = response.final_url = "https://fixture.example/report";
                response.completed_at = "2026-11-01T18:00:00Z";
                response.status = 200;
                response.headers["content-type"] = "text/html; charset=utf-8";
                response.body = "<html><head><title>Instrument calibration report</title></head>"
                                "<body><main><article><h1>Instrument calibration report</h1>";
                const std::string attributes =
                    decorated ? " class='calibration-reading-layout instrument-observation-content'"
                                " id='calibration-reading-section-detail' aria-hidden='false'"
                                " style='color: black; display: block'"
                              : "";
                for (unsigned i = 0; i < paragraphs; ++i)
                    response.body +=
                        "<p" + attributes +
                        ">The calibrated detector records the signal with its "
                        "uncertainty and measurement conditions. This fixture describes measurements "
                        "without claiming current market prices. <a rel='bookmark' href='/report#reading-" +
                        std::to_string(i) + "'>Source measurement</a></p>";
                response.body += "</article><div class='related-posts'>RELATED_NOISE</div>"
                                 "<div id='advertising-panel'>ADVERT_NOISE</div></main></body></html>";
                response.bytes = response.body.size();
                const auto expected = web::extract_document(response).dump();
                for (unsigned trial = 0; trial < 5; ++trial) {
                    std::uint64_t calls = 0, allocated = 0;
                    double milliseconds = 0;
                    for (unsigned iteration = 0; iteration < 20; ++iteration) {
                        const auto before = allocator_counters();
                        const auto began = Clock::now();
                        auto result = web::extract_document(response);
                        milliseconds +=
                            std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                        const auto after = allocator_counters();
                        calls += after.calls - before.calls;
                        allocated += after.allocated - before.allocated;
                        if (result.dump() != expected || result["document_type"] != "article" ||
                            json_string(result, "text").find("NOISE") != std::string::npos)
                            throw Error("Article selection or source evidence changed");
                    }
                    std::cout << Json{{"decorated", decorated},
                                      {"paragraphs", paragraphs},
                                      {"input_bytes", response.bytes},
                                      {"trial", trial},
                                      {"iterations", 20},
                                      {"milliseconds", milliseconds},
                                      {"allocation_calls", calls},
                                      {"allocated_bytes", allocated},
                                      {"output_sha256", sha256(expected)}}
                                     .dump()
                              << '\n';
                }
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
