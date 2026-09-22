#include "devbox/resource_budget.hpp"
#include <iostream>
#include <thread>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
int main() {
    try {
        ByteBudget budget(100);
        auto first = budget.acquire(80);
        bool denied = false;
        try {
            (void)budget.acquire(21);
        } catch (const ResourceExhausted&) {
            denied = true;
        }
        require(denied && budget.snapshot()["used_bytes"] == 80, "aggregate bytes rejected before admission");
        auto second = budget.acquire(20);
        first.reset();
        second.reset();
        require(budget.snapshot()["used_bytes"] == 0 && budget.snapshot()["peak_bytes"] == 100,
                "all reservation ownership released");
        const auto binary = Json::binary({0, 1, 255});
        const Json fixtures =
            Json::array({nullptr, false, 1.2, -0.0, UINT64_MAX, "é😀", std::string("a\xffz", 3),
                         Json{{"large", std::string(1024 * 1024, 'x')}, {"escape", "\n\t\""}}, binary});
        for (const auto& fixture : fixtures) {
            const auto expected = fixture.dump(-1, ' ', false, Json::error_handler_t::replace);
            require(bounded_json_dump(fixture, expected.size()) == expected,
                    "bounded writer preserves exact JSON and Unicode replacement");
            bool rejected = false;
            try {
                (void)bounded_json_dump(fixture, expected.size() - 1);
            } catch (const ResourceExhausted&) {
                rejected = true;
            }
            require(rejected, "bounded writer fails at the exact output boundary");
            require(json_memory_charge(fixture, 32 * 1024 * 1024) >= expected.size(),
                    "charge covers encoded output");
        }
        auto cancel = std::make_shared<Cancellation>();
        cancel->cancel();
        bool cancelled = false;
        try {
            (void)bounded_json_dump(fixtures, 16 * 1024 * 1024, cancel);
        } catch (const Cancelled&) {
            cancelled = true;
        }
        require(cancelled, "large encoding observes cancellation");
        std::cout << "Response byte accounting and bounded JSON serialization passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
