#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
int main() {
    try {
        std::cout << Json{{"build", build_snapshot()},
                          {"scope", "Complete document extraction; excludes network and browser time"}}
                         .dump()
                  << '\n';
        for (const auto* kind : {"ascii", "multilingual", "legacy", "html"}) {
            for (const std::size_t bytes : {131072U, 1048576U, 4194304U}) {
                web::Transfer transfer;
                transfer.url = transfer.final_url = "https://fixture.example/phone";
                transfer.status = 200;
                transfer.completed_at = "2026-10-31T20:00:00Z";
                transfer.headers["content-type"] =
                    std::string(kind) == "legacy" ? "text/plain; charset=windows-1252"
                    : std::string(kind) == "html" ? "text/html; charset=utf-8"
                                                  : "text/plain; charset=utf-8";
                std::string segment = "Phone price 123.45 GBP, checked product specification. ";
                if (std::string(kind) == "multilingual")
                    segment += "\xc2\xa3 \xe2\x82\xac \xe6\x89\x8b\xe6\x9c\xba \xf0\x9f\x93\xb1 ";
                if (std::string(kind) == "legacy")
                    segment += "\xa3 \x80 ";
                if (std::string(kind) == "html")
                    transfer.body = "<html><head><title>Phone fixture</title><script>const payload='";
                while (transfer.body.size() < bytes)
                    transfer.body += segment;
                if (std::string(kind) == "html")
                    transfer.body += "';</script></head><body><main><h1>Phone fixture</h1><p>" + segment +
                                     segment + segment + "</p></main></body></html>";
                transfer.bytes = transfer.body.size();
                const auto expected = web::extract_document(transfer).dump();
                for (unsigned trial = 0; trial < 5; ++trial) {
                    std::uint64_t allocations = 0, allocated = 0;
                    double milliseconds = 0;
                    for (unsigned iteration = 0; iteration < 10; ++iteration) {
                        const auto before = allocator_counters();
                        const auto started = Clock::now();
                        auto result = web::extract_document(transfer);
                        milliseconds +=
                            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                        const auto after = allocator_counters();
                        allocations += after.calls - before.calls;
                        allocated += after.allocated - before.allocated;
                        if (result.dump() != expected)
                            throw Error("Document evidence changed between iterations");
                    }
                    std::cout << Json{{"kind", kind},
                                      {"input_bytes", transfer.body.size()},
                                      {"trial", trial},
                                      {"iterations", 10},
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
