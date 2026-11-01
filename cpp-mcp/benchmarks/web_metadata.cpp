#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "Complete document extraction with product JSON-LD; no network"}}
                         .dump()
                  << '\n';
        for (const unsigned scripts : {1U, 8U}) {
            for (const unsigned records : {8U, 32U, 64U}) {
                web::Transfer response;
                response.url = response.final_url = "https://fixture.example/catalogue";
                response.completed_at = "2026-10-31T20:00:00Z";
                response.status = 200;
                response.headers["content-type"] = "text/html; charset=utf-8";
                response.body = "<html><head><title>Product catalogue</title>";
                for (unsigned script = 0; script < scripts; ++script) {
                    Json graph = Json::array();
                    for (unsigned i = script * records / scripts; i < (script + 1) * records / scripts; ++i)
                        graph.push_back(Json{{"@type", "Product"},
                                             {"name", "Phone " + std::to_string(i) + std::string(120, 'n')},
                                             {"description", std::string(192, 'd')}});
                    response.body += "<script type='application/ld+json'>" +
                                     Json{{"@graph", std::move(graph)}}.dump() + "</script>";
                }
                response.body += "</head><body><main><h1>Product catalogue</h1><p>These fixture phone "
                                 "descriptions are independently retained alongside their structured "
                                 "product metadata and source paths for bounded extraction testing. "
                                 "No synthetic price is advertised as a live offer.</p></main></body></html>";
                response.bytes = response.body.size();
                const auto expected = web::extract_document(response).dump();
                for (unsigned trial = 0; trial < 5; ++trial) {
                    std::uint64_t allocations = 0, allocated = 0;
                    double milliseconds = 0;
                    for (unsigned iteration = 0; iteration < 20; ++iteration) {
                        const auto before = allocator_counters();
                        const auto started = Clock::now();
                        auto result = web::extract_document(response);
                        milliseconds +=
                            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                        const auto after = allocator_counters();
                        allocations += after.calls - before.calls;
                        allocated += after.allocated - before.allocated;
                        if (result.dump() != expected || result["structured_metadata"].size() != records)
                            throw Error("Metadata evidence changed");
                    }
                    std::cout << Json{{"scripts", scripts},
                                      {"records", records},
                                      {"trial", trial},
                                      {"iterations", 20},
                                      {"milliseconds", milliseconds},
                                      {"allocation_calls", allocations},
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
