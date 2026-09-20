#include "tradebot/gateway/user_stream.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::gateway;
using namespace tradebot::execution;
using namespace tradebot::literals;

namespace {
const InstrumentId kEth{1};
}

TEST_CASE("parse_user_event: NEW, TRADE, CANCELED, REJECTED and non-report events") {
    auto neu = parse_user_event(
        R"({"e":"executionReport","E":1700000000100,"s":"ETHUSDT","c":"tb42","S":"BUY","o":"LIMIT","f":"GTC","q":"0.25000000","p":"3000.50000000","x":"NEW","X":"NEW","r":"NONE","i":12345,"l":"0","z":"0","L":"0","n":"0","N":null,"T":1700000000099,"t":-1,"m":false})",
        kEth);
    REQUIRE_MESSAGE(neu.has_value(), neu.error().message);
    REQUIRE(neu->has_value());
    const auto& a = (*neu)->report;
    CHECK((*neu)->execution_type == "NEW");
    CHECK(a.type == ReportType::accepted);
    CHECK(a.client_id == ClientOrderId{42});
    CHECK(a.order_id == OrderId{12345});
    CHECK(a.side == Side::buy);
    CHECK(a.order_type == OrderType::limit);
    CHECK(a.price == "3000.5"_px);
    CHECK(a.status == OrderStatus::open);
    CHECK(a.remaining_quantity == "0.25"_qty);
    CHECK(a.time == Timestamp::from_millis(1700000000099));
    CHECK(a.reason.empty());
    CHECK_FALSE(a.fill.has_value());

    auto trade = parse_user_event(
        R"({"e":"executionReport","E":1700000001000,"s":"ETHUSDT","c":"tb42","S":"BUY","o":"LIMIT","f":"GTC","q":"0.25","p":"3000.5","x":"TRADE","X":"PARTIALLY_FILLED","r":"NONE","i":12345,"l":"0.1","z":"0.1","L":"3000.4","n":"0.0001","N":"ETH","T":1700000000999,"t":777,"m":true})",
        kEth);
    REQUIRE(trade.has_value());
    const auto& t = (*trade)->report;
    CHECK(t.type == ReportType::fill);
    CHECK(t.status == OrderStatus::partially_filled);
    CHECK(t.filled_quantity == "0.1"_qty);
    CHECK(t.remaining_quantity == "0.15"_qty);
    REQUIRE(t.fill.has_value());
    CHECK(t.fill->price == "3000.4"_px);
    CHECK(t.fill->quantity == "0.1"_qty);
    CHECK(t.fill->fee == "0.0001"_ntl);
    CHECK(t.fill->liquidity == Liquidity::maker);
    CHECK(t.fill->exec_id == TradeId{777});
    CHECK((*trade)->commission_asset == "ETH");

    auto cancel = parse_user_event(
        R"({"e":"executionReport","E":1,"s":"ETHUSDT","c":"cancelreq1","C":"tb42","S":"BUY","o":"LIMIT","f":"GTC","q":"0.25","p":"3000.5","x":"CANCELED","X":"CANCELED","r":"NONE","i":12345,"l":"0","z":"0.1","L":"0","n":"0","T":2,"t":-1,"m":false})",
        kEth);
    REQUIRE(cancel.has_value());
    CHECK((*cancel)->report.type == ReportType::cancelled);
    CHECK((*cancel)->report.client_id == ClientOrderId{42});  // from "C"
    CHECK((*cancel)->report.status == OrderStatus::cancelled);

    auto rejected = parse_user_event(
        R"({"e":"executionReport","E":1,"s":"ETHUSDT","c":"tb43","S":"SELL","o":"LIMIT_MAKER","f":"GTC","q":"1","p":"1","x":"REJECTED","X":"REJECTED","r":"INSUFFICIENT_BALANCES","i":1,"l":"0","z":"0","L":"0","n":"0","T":2,"t":-1,"m":false})",
        kEth);
    REQUIRE(rejected.has_value());
    CHECK((*rejected)->report.type == ReportType::rejected);
    CHECK((*rejected)->report.reason == "INSUFFICIENT_BALANCES");

    auto other = parse_user_event(R"({"e":"outboundAccountPosition","E":1})", kEth);
    REQUIRE(other.has_value());
    CHECK_FALSE(other->has_value());
    auto foreign = parse_user_event(
        R"({"e":"executionReport","E":1,"s":"ETHUSDT","c":"web_abc","S":"BUY","o":"LIMIT","f":"GTC","q":"1","p":"1","x":"NEW","X":"NEW","r":"NONE","i":1,"l":"0","z":"0","L":"0","n":"0","T":2,"t":-1,"m":false})",
        kEth);
    REQUIRE(foreign.has_value());
    CHECK_FALSE((*foreign)->report.client_id.is_valid());  // not ours
    CHECK_FALSE(parse_user_event("nope", kEth).has_value());
    CHECK_FALSE(parse_user_event(R"({"e":"executionReport","c":"tb1"})", kEth).has_value());
}
