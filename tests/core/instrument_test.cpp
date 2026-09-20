#include "tradebot/core/instrument.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::literals;

namespace {
Instrument eth_usdt() {
    return Instrument{
        .id = InstrumentId{1},
        .venue = VenueId{1},
        .symbol = "ETHUSDT",
        .base = "ETH",
        .quote = "USDT",
        .tick_size = "0.01"_px,
        .lot_size = "0.0001"_qty,
        .min_quantity = "0.0001"_qty,
        .min_notional = "5"_ntl,
    };
}
}  // namespace

TEST_CASE("Instrument: definition validation") {
    CHECK(eth_usdt().validate_definition().has_value());
    auto bad = eth_usdt();
    bad.tick_size = Price{};
    CHECK_FALSE(bad.validate_definition().has_value());
    bad = eth_usdt();
    bad.lot_size = "-1"_qty;
    CHECK_FALSE(bad.validate_definition().has_value());
}

TEST_CASE("Instrument: price and quantity rules") {
    const auto inst = eth_usdt();
    CHECK(inst.is_valid_price("3000.12"_px));
    CHECK_FALSE(inst.is_valid_price("3000.123"_px));
    CHECK_FALSE(inst.is_valid_price(Price{}));
    CHECK(inst.is_valid_quantity("0.5"_qty));
    CHECK_FALSE(inst.is_valid_quantity("0.00005"_qty));  // below lot and min
    CHECK_FALSE(inst.is_valid_quantity("0.00015"_qty));  // not lot multiple
    CHECK_FALSE(inst.is_valid_quantity(Quantity{}));
}

TEST_CASE("Instrument: check_order enforces min notional") {
    const auto inst = eth_usdt();
    CHECK(inst.check_order("3000"_px, "0.01"_qty).has_value());  // 30 USDT
    auto r = inst.check_order("3000"_px, "0.001"_qty);  // 3 USDT < 5
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::invalid_argument);
    CHECK_FALSE(inst.check_order("3000.001"_px, "0.01"_qty).has_value());
    CHECK_FALSE(inst.check_order("3000"_px, "0.00001"_qty).has_value());
}

TEST_CASE("Instrument: rounding helpers") {
    const auto inst = eth_usdt();
    CHECK(inst.round_price("3000.129"_px, RoundingMode::down) == "3000.12"_px);
    CHECK(inst.round_price("3000.121"_px, RoundingMode::up) == "3000.13"_px);
    CHECK(inst.round_quantity("0.12345"_qty, RoundingMode::down) == "0.1234"_qty);
}
