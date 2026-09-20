#include "tradebot/market_data/binance/csv.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::market_data::binance;
using namespace tradebot::literals;

namespace {
const InstrumentId kEth{1};
}

TEST_CASE("epoch_from_ms_or_us") {
    CHECK(epoch_from_ms_or_us(1704067200000) == Timestamp::from_millis(1704067200000));
    CHECK(epoch_from_ms_or_us(1704067200000000) == Timestamp::from_micros(1704067200000000));
    CHECK(epoch_from_ms_or_us(1704067200000) == epoch_from_ms_or_us(1704067200000000));
}

TEST_CASE("is_csv_header") {
    CHECK(is_csv_header("agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker,is_best_match"));
    CHECK_FALSE(is_csv_header("100,3000.00,0.1000,200,200,1704067200000,false,true"));
    CHECK_FALSE(is_csv_header(""));
}

TEST_CASE("parse_agg_trade_csv") {
    auto t = parse_agg_trade_csv("100,3000.00,0.1000,200,203,1704067200000,true,true", kEth);
    REQUIRE_MESSAGE(t.has_value(), t.error().message);
    CHECK(t->id == TradeId{100});
    CHECK(t->price == "3000"_px);
    CHECK(t->quantity == "0.1"_qty);
    CHECK(t->first_trade_id == 200);
    CHECK(t->last_trade_id == 203);
    CHECK(t->exchange_time == *Timestamp::parse_iso8601("2024-01-01"));
    CHECK(t->recv_time == t->exchange_time);
    CHECK(t->aggressor == Side::sell);

    // Microsecond timestamps and capitalized booleans (2025+ archives).
    t = parse_agg_trade_csv("101,3000.50,0.2,201,201,1704067201000000,False,True", kEth);
    REQUIRE(t.has_value());
    CHECK(t->exchange_time == *Timestamp::parse_iso8601("2024-01-01T00:00:01Z"));
    CHECK(t->aggressor == Side::buy);

    CHECK_FALSE(parse_agg_trade_csv("100,3000.00,0.1000,200,203,1704067200000", kEth).has_value());
    CHECK_FALSE(parse_agg_trade_csv("x,3000.00,0.1000,200,203,1704067200000,true,true", kEth).has_value());
    CHECK_FALSE(parse_agg_trade_csv("100,abc,0.1000,200,203,1704067200000,true,true", kEth).has_value());
    CHECK_FALSE(parse_agg_trade_csv("100,3000,0.1,200,203,1704067200000,maybe,true", kEth).has_value());
}

TEST_CASE("parse_trade_csv") {
    auto t = parse_trade_csv("777,3000.25,2.0,6000.5,1704067200500,false,true", kEth);
    REQUIRE_MESSAGE(t.has_value(), t.error().message);
    CHECK(t->id == TradeId{777});
    CHECK(t->price == "3000.25"_px);
    CHECK(t->quantity == "2"_qty);
    CHECK(t->exchange_time == Timestamp::from_millis(1704067200500));
    CHECK(t->aggressor == Side::buy);
    CHECK(t->first_trade_id == 777);
    CHECK(t->last_trade_id == 777);
    CHECK_FALSE(parse_trade_csv("777,3000.25,2.0,6000.5", kEth).has_value());
}

TEST_CASE("parse_kline_csv") {
    const char* line = "1704067200000,2281.87,2284.00,2280.10,2283.50,150.1234,1704067259999,342612.45,1200,80.5,183700.12,0";
    auto c = parse_kline_csv(line, kEth, Duration::minutes(1));
    REQUIRE_MESSAGE(c.has_value(), c.error().message);
    CHECK(c->open_time == *Timestamp::parse_iso8601("2024-01-01"));
    CHECK(c->interval == Duration::minutes(1));
    CHECK(c->close_time() == *Timestamp::parse_iso8601("2024-01-01T00:01:00Z"));
    CHECK(c->recv_time == c->close_time());
    CHECK(c->open == "2281.87"_px);
    CHECK(c->high == "2284"_px);
    CHECK(c->low == "2280.1"_px);
    CHECK(c->close == "2283.5"_px);
    CHECK(c->volume == "150.1234"_qty);
    CHECK(c->quote_volume == "342612.45"_ntl);
    CHECK(c->trade_count == 1200);
    CHECK(c->taker_buy_volume == "80.5"_qty);
    CHECK(c->closed);

    // Microsecond variant.
    c = parse_kline_csv("1704067200000000,1,2,0.5,1.5,10,1704067259999999,15,3,5,7.5,0", kEth, Duration::minutes(1));
    REQUIRE(c.has_value());
    CHECK(c->open_time == *Timestamp::parse_iso8601("2024-01-01"));

    CHECK_FALSE(parse_kline_csv("1704067200000,1,2,0.5,1.5,10", kEth, Duration::minutes(1)).has_value());
    CHECK_FALSE(parse_kline_csv("open_time,open,high,low,close,volume,close_time,quote_volume,count,taker_buy_volume,taker_buy_quote_volume,ignore", kEth, Duration::minutes(1)).has_value());
}
