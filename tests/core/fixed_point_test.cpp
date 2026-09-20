#include "tradebot/core/fixed_point.hpp"

#include <doctest/doctest.h>

#include <limits>
#include <stdexcept>

using namespace tradebot;
using namespace tradebot::literals;

TEST_CASE("Fixed: parse and format round-trip exactly") {
    const char* samples[] = {"0",      "1",          "-1",         "1234.5",  "0.00000001",
                             "-0.5",   "92233720368", "3000.12345678", "0.1", "100.00"};
    for (const char* s : samples) {
        auto p = Price::parse(s);
        REQUIRE_MESSAGE(p.has_value(), s);
        auto back = Price::parse(p->to_string());
        REQUIRE(back.has_value());
        CHECK(*back == *p);
    }
    CHECK(Price::parse("100.00")->to_string() == "100");
    CHECK(Price::parse("0.10")->to_string() == "0.1");
    CHECK(Price::parse("-0.00000001")->to_string() == "-0.00000001");
    CHECK(Price::from_raw(std::numeric_limits<std::int64_t>::min()).to_string() ==
          "-92233720368.54775808");
}

TEST_CASE("Fixed: parse rejects garbage and excess precision") {
    CHECK_FALSE(Price::parse("").has_value());
    CHECK_FALSE(Price::parse("abc").has_value());
    CHECK_FALSE(Price::parse("1.2.3").has_value());
    CHECK_FALSE(Price::parse("1e5").has_value());
    CHECK_FALSE(Price::parse("-").has_value());
    CHECK_FALSE(Price::parse(".").has_value());
    CHECK_FALSE(Price::parse("0.123456789").has_value());  // 9 digits
    CHECK(Price::parse("0.123456780").has_value());  // trailing zero ok
    CHECK_FALSE(Price::parse("99999999999").has_value());  // overflow
    CHECK(Price::parse("1.").has_value());
    CHECK(Price::parse(".5").has_value());
    CHECK(Price::parse(".5")->raw() == 50'000'000);
}

TEST_CASE("Fixed: arithmetic is exact") {
    CHECK(("0.1"_px + "0.2"_px) == "0.3"_px);
    CHECK(("1"_px - "0.00000001"_px) == "0.99999999"_px);
    CHECK(("2.5"_qty * 3) == "7.5"_qty);
    CHECK((-"2.5"_qty) == "-2.5"_qty);
    CHECK("-2.5"_qty.abs() == "2.5"_qty);
    CHECK("0.1"_px < "0.2"_px);
    CHECK("0.1"_px.is_positive());
    CHECK("-0.1"_px.is_negative());
    CHECK(Price{}.is_zero());
}

TEST_CASE("Fixed: cross-type products") {
    // 3000.5 * 0.25 = 750.125
    CHECK(notional("3000.5"_px, "0.25"_qty) == "750.125"_ntl);
    // 750.125 / 3000.5 = 0.25
    CHECK(quantity_for("750.125"_ntl, "3000.5"_px) == "0.25"_qty);
    CHECK(price_for("750.125"_ntl, "0.25"_qty) == "3000.5"_px);
    // Rounding of an inexact quotient: 100 / 3 = 33.33333333...
    CHECK(quantity_for("100"_ntl, "3"_px, RoundingMode::down) == "33.33333333"_qty);
    CHECK(quantity_for("100"_ntl, "3"_px, RoundingMode::up) == "33.33333334"_qty);
    CHECK(quantity_for("100"_ntl, "3"_px, RoundingMode::nearest) == "33.33333333"_qty);
    CHECK(quantity_for("200"_ntl, "3"_px, RoundingMode::nearest) == "66.66666667"_qty);
    // Product below resolution rounds explicitly.
    CHECK(notional("0.00000001"_px, "0.00000001"_qty, RoundingMode::nearest).is_zero());
    CHECK(notional("0.00000001"_px, "0.00000001"_qty, RoundingMode::up) ==
          Notional::from_raw(1));
}

TEST_CASE("Fixed: mul_ratio for fees") {
    // 0.1% taker fee on 1234.5678 notional = 1.2345678
    CHECK("1234.5678"_ntl.mul_ratio(1, 1000) == "1.2345678"_ntl);
    // Half-away-from-zero rounding at the last digit
    CHECK("0.00000005"_ntl.mul_ratio(1, 10, RoundingMode::nearest) == Notional::from_raw(1));
    CHECK("0.00000004"_ntl.mul_ratio(1, 10, RoundingMode::nearest) == Notional::from_raw(0));
    CHECK("-0.00000005"_ntl.mul_ratio(1, 10, RoundingMode::nearest) == Notional::from_raw(-1));
    CHECK("-0.00000005"_ntl.mul_ratio(1, 10, RoundingMode::down) == Notional::from_raw(-1));
    CHECK("-0.00000005"_ntl.mul_ratio(1, 10, RoundingMode::up) == Notional::from_raw(0));
}

TEST_CASE("Fixed: round_to tick/lot") {
    const Price tick = "0.01"_px;
    CHECK("1234.5678"_px.round_to(tick, RoundingMode::down) == "1234.56"_px);
    CHECK("1234.5678"_px.round_to(tick, RoundingMode::up) == "1234.57"_px);
    CHECK("1234.5650"_px.round_to(tick, RoundingMode::nearest) == "1234.57"_px);
    CHECK("1234.5649"_px.round_to(tick, RoundingMode::nearest) == "1234.56"_px);
    CHECK("-1234.5678"_px.round_to(tick, RoundingMode::down) == "-1234.57"_px);
    CHECK("-1234.5678"_px.round_to(tick, RoundingMode::up) == "-1234.56"_px);
    CHECK("1234.56"_px.is_multiple_of(tick));
    CHECK_FALSE("1234.565"_px.is_multiple_of(tick));
    CHECK_THROWS_AS(static_cast<void>("1"_px.round_to(Price{}, RoundingMode::down)), std::invalid_argument);
}

TEST_CASE("Fixed: overflow throws instead of wrapping") {
    const auto big = Price::from_raw(std::numeric_limits<std::int64_t>::max());
    CHECK_THROWS_AS(static_cast<void>(big + "0.00000001"_px), std::overflow_error);
    CHECK_THROWS_AS(static_cast<void>(big * 2), std::overflow_error);
    CHECK_THROWS_AS(static_cast<void>(Price::from_int(100'000'000'000)), std::overflow_error);
    CHECK_THROWS_AS(static_cast<void>(-Price::from_raw(std::numeric_limits<std::int64_t>::min())),
                    std::overflow_error);
    CHECK_THROWS_AS(static_cast<void>(notional(big, "2"_qty)), std::overflow_error);
    CHECK_THROWS_AS(static_cast<void>(quantity_for("1"_ntl, Price{})), std::domain_error);
}

TEST_CASE("Fixed: double conversion at the boundary") {
    CHECK(Price::from_double(3000.12345678) == "3000.12345678"_px);
    CHECK(Price::from_double(0.1) == "0.1"_px);  // rounds away the binary error
    CHECK(Price::from_double(-2.5) == "-2.5"_px);
    CHECK("3000.5"_px.to_double() == doctest::Approx(3000.5));
    CHECK_THROWS_AS(static_cast<void>(Price::from_double(1e12)), std::overflow_error);
}

TEST_CASE("Fixed: ratio") {
    CHECK(ratio("1"_qty, "4"_qty) == doctest::Approx(0.25));
}
