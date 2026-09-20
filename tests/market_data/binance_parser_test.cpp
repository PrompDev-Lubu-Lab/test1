#include "tradebot/market_data/binance/parser.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::market_data::binance;
using namespace tradebot::literals;

namespace {
const InstrumentId kEth{1};
const Timestamp kRecv = *Timestamp::parse_iso8601("2024-03-15T12:00:00.5Z");
}  // namespace

TEST_CASE("parse_stream_message: aggTrade (combined wrapper)") {
    const std::string msg =
        R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","E":1710504000123,"s":"ETHUSDT",)"
        R"("a":1234567,"p":"3000.12000000","q":"0.25000000","f":100,"l":103,"T":1710504000120,"m":true,"M":true}})";
    auto ev = parse_stream_message(msg, kEth, kRecv);
    REQUIRE_MESSAGE(ev.has_value(), ev.error().message);
    REQUIRE(std::holds_alternative<Trade>(*ev));
    const auto& t = std::get<Trade>(*ev);
    CHECK(t.instrument == kEth);
    CHECK(t.recv_time == kRecv);
    CHECK(t.exchange_time == Timestamp::from_millis(1710504000120));
    CHECK(t.id == TradeId{1234567});
    CHECK(t.price == "3000.12"_px);
    CHECK(t.quantity == "0.25"_qty);
    CHECK(t.aggressor == Side::sell);  // buyer was maker => seller took
    CHECK(t.first_trade_id == 100);
    CHECK(t.last_trade_id == 103);
    CHECK(event_time(*ev) == kRecv);
    CHECK(event_type_name(*ev) == "trade");
}

TEST_CASE("parse_stream_message: trade (bare payload)") {
    const std::string msg =
        R"({"e":"trade","E":1710504000123,"s":"ETHUSDT","t":555,"p":"2999.99","q":"1.5","T":1710504000100,"m":false,"M":true})";
    auto ev = parse_stream_message(msg, kEth, kRecv);
    REQUIRE(ev.has_value());
    const auto& t = std::get<Trade>(*ev);
    CHECK(t.id == TradeId{555});
    CHECK(t.aggressor == Side::buy);
    CHECK(t.first_trade_id == 555);
    CHECK(t.last_trade_id == 555);
}

TEST_CASE("parse_stream_message: depthUpdate") {
    const std::string msg =
        R"({"stream":"ethusdt@depth@100ms","data":{"e":"depthUpdate","E":1710504000200,"s":"ETHUSDT",)"
        R"("U":157,"u":160,"b":[["3000.00","1.5"],["2999.99","0"]],"a":[["3000.01","2.25"]]}})";
    auto ev = parse_stream_message(msg, kEth, kRecv);
    REQUIRE_MESSAGE(ev.has_value(), ev.error().message);
    REQUIRE(std::holds_alternative<BookDelta>(*ev));
    const auto& d = std::get<BookDelta>(*ev);
    CHECK(d.first_update_id == 157);
    CHECK(d.final_update_id == 160);
    CHECK(d.exchange_time == Timestamp::from_millis(1710504000200));
    REQUIRE(d.bids.size() == 2);
    CHECK(d.bids[0].price == "3000"_px);
    CHECK(d.bids[0].quantity == "1.5"_qty);
    CHECK(d.bids[1].quantity.is_zero());  // removal
    REQUIRE(d.asks.size() == 1);
    CHECK(d.asks[0].price == "3000.01"_px);
}

TEST_CASE("parse_stream_message: kline") {
    const std::string msg =
        R"({"e":"kline","E":1710504060001,"s":"ETHUSDT","k":{"t":1710504000000,"T":1710504059999,"s":"ETHUSDT",)"
        R"("i":"1m","f":100,"L":200,"o":"3000.0","c":"3005.5","h":"3006","l":"2999","v":"12.5","n":101,"x":true,)"
        R"("q":"37531.25","V":"7.5","Q":"22518.75","B":"0"}})";
    auto ev = parse_stream_message(msg, kEth, kRecv);
    REQUIRE_MESSAGE(ev.has_value(), ev.error().message);
    REQUIRE(std::holds_alternative<Candle>(*ev));
    const auto& c = std::get<Candle>(*ev);
    CHECK(c.open_time == Timestamp::from_millis(1710504000000));
    CHECK(c.interval == Duration::minutes(1));
    CHECK(c.close_time() == Timestamp::from_millis(1710504060000));
    CHECK(c.open == "3000"_px);
    CHECK(c.high == "3006"_px);
    CHECK(c.low == "2999"_px);
    CHECK(c.close == "3005.5"_px);
    CHECK(c.volume == "12.5"_qty);
    CHECK(c.quote_volume == "37531.25"_ntl);
    CHECK(c.taker_buy_volume == "7.5"_qty);
    CHECK(c.trade_count == 101);
    CHECK(c.closed);
}

TEST_CASE("parse_stream_message: bookTicker") {
    const std::string msg =
        R"({"stream":"ethusdt@bookTicker","data":{"u":400900217,"s":"ETHUSDT","b":"3000.00","B":"3.1","a":"3000.01","A":"4.2"}})";
    auto ev = parse_stream_message(msg, kEth, kRecv);
    REQUIRE_MESSAGE(ev.has_value(), ev.error().message);
    REQUIRE(std::holds_alternative<BookTicker>(*ev));
    const auto& t = std::get<BookTicker>(*ev);
    CHECK(t.update_id == 400900217);
    CHECK(t.bid_price == "3000"_px);
    CHECK(t.bid_quantity == "3.1"_qty);
    CHECK(t.ask_price == "3000.01"_px);
    CHECK(t.ask_quantity == "4.2"_qty);
}

TEST_CASE("parse_stream_message: strictness") {
    // Missing field
    CHECK_FALSE(parse_stream_message(R"({"e":"aggTrade","a":1,"p":"1","q":"1","f":1,"l":1,"T":1})", kEth, kRecv).has_value());
    // Wrong type (price as number, not string)
    CHECK_FALSE(parse_stream_message(R"({"e":"aggTrade","a":1,"p":1.0,"q":"1","f":1,"l":1,"T":1,"m":true})", kEth, kRecv).has_value());
    // Bad decimal
    CHECK_FALSE(parse_stream_message(R"({"e":"aggTrade","a":1,"p":"1e3","q":"1","f":1,"l":1,"T":1,"m":true})", kEth, kRecv).has_value());
    // Unknown event type is 'unsupported' (ignorable), garbage is parse_error
    auto unsupported = parse_stream_message(R"({"e":"24hrTicker","s":"ETHUSDT"})", kEth, kRecv);
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error().code == ErrorCode::unsupported);
    auto garbage = parse_stream_message("not json", kEth, kRecv);
    REQUIRE_FALSE(garbage.has_value());
    CHECK(garbage.error().code == ErrorCode::parse_error);
    CHECK_FALSE(parse_stream_message(R"({"hello":"world"})", kEth, kRecv).has_value());
    CHECK_FALSE(parse_stream_message(R"([1,2,3])", kEth, kRecv).has_value());
    // Malformed level
    CHECK_FALSE(parse_stream_message(R"({"e":"depthUpdate","E":1,"U":1,"u":1,"b":[["x"]],"a":[]})", kEth, kRecv).has_value());
}

TEST_CASE("parse_depth_snapshot") {
    const std::string body =
        R"({"lastUpdateId":1027024,"bids":[["3000.00","4.5"],["2999.50","10"]],"asks":[["3000.10","1"],["3001.00","2"]]})";
    auto s = parse_depth_snapshot(body, kEth, kRecv);
    REQUIRE_MESSAGE(s.has_value(), s.error().message);
    CHECK(s->last_update_id == 1027024);
    CHECK(s->recv_time == kRecv);
    REQUIRE(s->bids.size() == 2);
    REQUIRE(s->asks.size() == 2);
    CHECK(s->bids[1].price == "2999.5"_px);
    CHECK(s->asks[1].quantity == "2"_qty);
    CHECK_FALSE(parse_depth_snapshot(R"({"bids":[],"asks":[]})", kEth, kRecv).has_value());
    CHECK_FALSE(parse_depth_snapshot(R"({"lastUpdateId":1,"bids":{}})", kEth, kRecv).has_value());
}

TEST_CASE("parse_exchange_info") {
    const std::string body = R"({"timezone":"UTC","symbols":[
        {"symbol":"BTCUSDT","baseAsset":"BTC","quoteAsset":"USDT","filters":[]},
        {"symbol":"ETHUSDT","baseAsset":"ETH","quoteAsset":"USDT","status":"TRADING","filters":[
            {"filterType":"PRICE_FILTER","minPrice":"0.01000000","maxPrice":"1000000.00000000","tickSize":"0.01000000"},
            {"filterType":"LOT_SIZE","minQty":"0.00010000","maxQty":"9000.00000000","stepSize":"0.00010000"},
            {"filterType":"NOTIONAL","minNotional":"5.00000000","applyMinToMarket":true,"maxNotional":"9000000.00000000"}
        ]}]})";
    auto inst = parse_exchange_info(body, "ETHUSDT", kEth, VenueId{7});
    REQUIRE_MESSAGE(inst.has_value(), inst.error().message);
    CHECK(inst->id == kEth);
    CHECK(inst->venue == VenueId{7});
    CHECK(inst->symbol == "ETHUSDT");
    CHECK(inst->base == "ETH");
    CHECK(inst->quote == "USDT");
    CHECK(inst->tick_size == "0.01"_px);
    CHECK(inst->lot_size == "0.0001"_qty);
    CHECK(inst->min_quantity == "0.0001"_qty);
    CHECK(inst->min_notional == "5"_ntl);
    CHECK(inst->check_order("3000"_px, "0.01"_qty).has_value());

    auto missing = parse_exchange_info(body, "SOLUSDT", kEth, VenueId{7});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::not_found);
    // BTCUSDT has no filters -> error
    CHECK_FALSE(parse_exchange_info(body, "BTCUSDT", kEth, VenueId{7}).has_value());
}

TEST_CASE("kline intervals") {
    CHECK(*parse_kline_interval("1s") == Duration::seconds(1));
    CHECK(*parse_kline_interval("1m") == Duration::minutes(1));
    CHECK(*parse_kline_interval("15m") == Duration::minutes(15));
    CHECK(*parse_kline_interval("4h") == Duration::hours(4));
    CHECK(*parse_kline_interval("1d") == Duration::days(1));
    CHECK(*parse_kline_interval("1w") == Duration::days(7));
    CHECK_FALSE(parse_kline_interval("1M").has_value());
    CHECK_FALSE(parse_kline_interval("m").has_value());
    CHECK_FALSE(parse_kline_interval("0m").has_value());
    CHECK(kline_interval_name(Duration::minutes(5)) == "5m");
    CHECK(kline_interval_name(Duration::hours(4)) == "4h");
    CHECK(kline_interval_name(Duration::days(1)) == "1d");
    CHECK(kline_interval_name(Duration::days(7)) == "1w");
    CHECK(kline_interval_name(Duration::seconds(30)) == "30s");
}
