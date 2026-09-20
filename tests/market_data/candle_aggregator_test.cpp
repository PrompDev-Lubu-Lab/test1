#include "tradebot/market_data/candle_aggregator.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

Trade trade(Duration offset, const char* price, const char* qty, Side side = Side::buy) {
    Trade t;
    t.instrument = kEth;
    t.exchange_time = kT0 + offset;
    t.recv_time = t.exchange_time + Duration::millis(3);
    t.id = TradeId{static_cast<std::uint64_t>(offset.count_millis())};
    t.price = *Price::parse(price);
    t.quantity = *Quantity::parse(qty);
    t.aggressor = side;
    return t;
}

}  // namespace

TEST_CASE("CandleAggregator: OHLCV over one interval") {
    std::vector<Candle> closed;
    CandleAggregator agg(kEth, {.intervals = {Duration::minutes(1)}, .fill_gaps = false},
                         [&](const Candle& c) { closed.push_back(c); });

    agg.on_trade(trade(Duration::seconds(1), "100", "1", Side::buy));
    agg.on_trade(trade(Duration::seconds(20), "105", "2", Side::sell));
    agg.on_trade(trade(Duration::seconds(40), "95", "1", Side::buy));
    agg.on_trade(trade(Duration::seconds(59), "102", "0.5", Side::buy));
    CHECK(closed.empty());
    auto cur = agg.current(Duration::minutes(1));
    REQUIRE(cur.has_value());
    CHECK_FALSE(cur->closed);
    CHECK(cur->open_time == kT0);
    CHECK(cur->open == "100"_px);
    CHECK(cur->high == "105"_px);
    CHECK(cur->low == "95"_px);
    CHECK(cur->close == "102"_px);
    CHECK(cur->volume == "4.5"_qty);
    CHECK(cur->taker_buy_volume == "2.5"_qty);
    CHECK(cur->trade_count == 4);
    // 100*1 + 105*2 + 95*1 + 102*0.5 = 456
    CHECK(cur->quote_volume == "456"_ntl);

    // Trade in the next minute closes the first candle.
    agg.on_trade(trade(Duration::seconds(61), "110", "1"));
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].closed);
    CHECK(closed[0].close == "102"_px);
    CHECK(closed[0].recv_time == kT0 + Duration::minutes(1));
    cur = agg.current(Duration::minutes(1));
    REQUIRE(cur.has_value());
    CHECK(cur->open_time == kT0 + Duration::minutes(1));
    CHECK(cur->open == "110"_px);
    CHECK(cur->trade_count == 1);
}

TEST_CASE("CandleAggregator: gap filling and multiple intervals") {
    std::vector<Candle> closed;
    CandleAggregator agg(kEth, {.intervals = {Duration::minutes(1), Duration::minutes(5)}},
                         [&](const Candle& c) { closed.push_back(c); });

    agg.on_trade(trade(Duration::seconds(10), "100", "1"));
    // Next trade 3 minutes and 30 seconds later: 1m series closes 10:00,
    // fills 10:01 and 10:02, then opens 10:03. 5m series stays in 10:00.
    agg.on_trade(trade(Duration::seconds(210), "104", "1"));
    REQUIRE(closed.size() == 3);
    CHECK(closed[0].interval == Duration::minutes(1));
    CHECK(closed[0].open_time == kT0);
    CHECK(closed[0].close == "100"_px);
    CHECK(closed[1].open_time == kT0 + Duration::minutes(1));
    CHECK(closed[1].open == "100"_px);
    CHECK(closed[1].close == "100"_px);
    CHECK(closed[1].volume.is_zero());
    CHECK(closed[1].trade_count == 0);
    CHECK(closed[1].closed);
    CHECK(closed[2].open_time == kT0 + Duration::minutes(2));
    auto five = agg.current(Duration::minutes(5));
    REQUIRE(five.has_value());
    CHECK(five->open_time == kT0);
    CHECK(five->high == "104"_px);
    CHECK(five->trade_count == 2);

    // Advancing the clock without trades closes both series up to now.
    closed.clear();
    agg.advance_to(kT0 + Duration::minutes(7));
    // 1m: closes 10:03 (has trade), fills 10:04, 10:05, 10:06 -> 4 candles.
    // 5m: closes 10:00-10:05, fills none (10:05 bucket contains 'until'? no: target_open = 10:05) -> 1 candle.
    REQUIRE(closed.size() == 5);
    int one_min = 0, five_min = 0;
    for (const auto& c : closed) {
        if (c.interval == Duration::minutes(1)) ++one_min;
        else ++five_min;
    }
    CHECK(one_min == 4);
    CHECK(five_min == 1);
    CHECK_FALSE(agg.current(Duration::minutes(1)).has_value());
    CHECK_FALSE(agg.current(Duration::minutes(5)).has_value());

    // Next trade after a quiet period opens a new bucket with the gap filled
    // between the last closed bucket and the new one.
    closed.clear();
    agg.on_trade(trade(Duration::minutes(9), "90", "1"));
    // 1m series: last emitted 10:06, needs 10:07 and 10:08 fills? No: fills
    // are emitted when a bucket closes, and nothing was open. The new bucket
    // opens at 10:09 with previous close carried as reference only.
    CHECK(closed.empty());
    auto cur = agg.current(Duration::minutes(1));
    REQUIRE(cur.has_value());
    CHECK(cur->open == "90"_px);
}

TEST_CASE("CandleAggregator: late trades are counted and ignored") {
    std::vector<Candle> closed;
    CandleAggregator agg(kEth, {.intervals = {Duration::minutes(1)}, .fill_gaps = false},
                         [&](const Candle& c) { closed.push_back(c); });
    agg.on_trade(trade(Duration::seconds(70), "100", "1"));
    agg.on_trade(trade(Duration::seconds(5), "50", "1"));  // belongs to a closed/earlier bucket
    CHECK(agg.late_trades() == 1);
    CHECK(agg.current(Duration::minutes(1))->low == "100"_px);
    // Same-bucket out-of-order trade is still applied.
    agg.on_trade(trade(Duration::seconds(65), "99", "1"));
    CHECK(agg.late_trades() == 1);
    CHECK(agg.current(Duration::minutes(1))->low == "99"_px);
}
