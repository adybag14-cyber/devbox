#include "devbox/common.hpp"
#include <iostream>
#include <limits>
#include <locale>
#include <random>
using namespace devbox;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
struct GroupedDigits : std::numpunct<char> {
    char do_thousands_sep() const override {
        return '_';
    }
    std::string do_grouping() const override {
        return "\1";
    }
};
int main() {
    try {
        for (const auto& [milliseconds, expected] : std::vector<std::pair<std::int64_t, std::string>>{
                 {0, "1970-01-01T00:00:00.000Z"},
                 {123, "1970-01-01T00:00:00.123Z"},
                 {-1, "1969-12-31T23:59:59.999Z"},
                 {-1000, "1969-12-31T23:59:59.000Z"},
                 {-86400000, "1969-12-31T00:00:00.000Z"},
                 {-62167219200000LL, "0000-01-01T00:00:00.000Z"},
                 {-62135596800000LL, "0001-01-01T00:00:00.000Z"},
                 {-2203891200000LL, "1900-03-01T00:00:00.000Z"},
                 {951782400000LL, "2000-02-29T00:00:00.000Z"},
                 {4107542400000LL, "2100-03-01T00:00:00.000Z"},
                 {253402300799999LL, "9999-12-31T23:59:59.999Z"}}) {
            const auto formatted = utc_from_millis(milliseconds);
            require(formatted == expected,
                    "UTC fields and milliseconds match the Gregorian boundary fixture");
            require(parse_utc(formatted) == milliseconds, "UTC formatting round-trips exact milliseconds");
        }
        for (const auto invalid :
             {-62167219200001LL, 253402300800000LL, std::numeric_limits<long long>::min(),
              std::numeric_limits<long long>::max()}) {
            bool rejected = false;
            try {
                (void)utc_from_millis(invalid);
            } catch (const Error& error) {
                rejected = std::string_view(error.what()) == "UTC_TIMESTAMP_OUT_OF_RANGE";
            }
            require(rejected, "Out-of-range timestamps fail explicitly without calendar overflow");
        }
        std::mt19937_64 random(930);
        constexpr std::int64_t minimum = -62167219200000LL, span = 315569520000000LL;
        for (unsigned i = 0; i < 10000; ++i) {
            const auto value =
                minimum + static_cast<std::int64_t>(random() % static_cast<std::uint64_t>(span));
            const auto formatted = utc_from_millis(value);
            require(formatted.size() == 24 && parse_utc(formatted) == value,
                    "All supported UTC years retain fixed-width round-trip precision");
        }
        const auto previous = std::locale();
        ScopeExit restore([&] { std::locale::global(previous); });
        std::locale::global(std::locale(previous, new GroupedDigits));
        require(utc_from_millis(123) == "1970-01-01T00:00:00.123Z",
                "Protocol timestamps must not inherit localized numeric grouping");
        std::cout << "UTC boundaries, negative epochs, range and locale independence passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
