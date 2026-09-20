#include "tradebot/market_data/validation.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::literals;

namespace {

Trade trade(std::uint64_t id, std::int64_t ms, const char* price = "100", const char* qty = "1") {
    Trade t;
    t.instrument = InstrumentId{1};
    t.exchange_time = Timestamp::from_millis(ms);
    t.recv_time = t.exchange_time;
    t.id = TradeId{id};
    t.price = *Price::parse(price);
    t.quantity = *Quantity::parse(qty);
    t.aggressor = Side::buy;
    return t;
}

BookTicker ticker(std::int64_t id, const char* bid, const char* ask, const char* bq = "1",
                  const char* aq = "1") {
    BookTicker t;
    t.instrument = InstrumentId{1};
    t.recv_time = Timestamp::from_millis(id);
    t.update_id = id;
    t.bid_price = *Price::parse(bid);
    t.ask_price = *Price::parse(ask);
    t.bid_quantity = *Quantity::parse(bq);
    t.ask_quantity = *Quantity::parse(aq);
    return t;
}

}  // namespace

TEST_CASE("TradeValidator: duplicates dropped, gaps and regressions flagged, bad values dropped") {
    std::vector<ValidationIssue> issues;
    TradeValidator v([&](const ValidationIssue& i) { issues.push_back(i); });

    CHECK(v.check(trade(10, 1000)));
    CHECK(v.check(trade(11, 1000)));
    CHECK_FALSE(v.check(trade(11, 1001)));  // duplicate
    CHECK_FALSE(v.check(trade(5, 1001)));  // older
    CHECK(v.check(trade(15, 1002)));  // gap 12..14, kept
    CHECK(v.check(trade(16, 900)));  // time regression, kept
    CHECK_FALSE(v.check(trade(17, 1003, "0", "1")));
    CHECK_FALSE(v.check(trade(18, 1003, "1", "-1")));
    CHECK(v.check(trade(19, 1003)));

    const auto& s = v.stats();
    CHECK(s.accepted == 5);
    CHECK(s.dropped == 4);
    CHECK(s.duplicates == 2);
    CHECK(s.gaps == 1);
    CHECK(s.time_regressions == 1);
    CHECK(s.invalid_values == 2);
    REQUIRE(issues.size() == 6);
    CHECK(issues[0].kind == IssueKind::duplicate);
    CHECK(issues[2].kind == IssueKind::gap);
    CHECK(issues[2].detail.find("11 to 15") != std::string::npos);
    CHECK(issues[3].kind == IssueKind::time_regression);
    CHECK(to_string(issues[4].kind) == "invalid_value");

    // After reset, ids may restart without being duplicates.
    v.reset();
    CHECK(v.check(trade(1, 1)));
}

TEST_CASE("BookTickerValidator: crossed or empty books and stale ids are dropped") {
    BookTickerValidator v;
    CHECK(v.check(ticker(1, "100", "100.01")));
    CHECK_FALSE(v.check(ticker(1, "100", "100.01")));  // same id
    CHECK_FALSE(v.check(ticker(2, "100.02", "100.01")));  // crossed
    CHECK_FALSE(v.check(ticker(3, "100", "100")));  // locked
    CHECK_FALSE(v.check(ticker(4, "100", "100.01", "0", "1")));  // empty bid
    CHECK(v.check(ticker(5, "100", "100.01")));
    CHECK(v.stats().accepted == 2);
    CHECK(v.stats().dropped == 4);
    CHECK(v.stats().duplicates == 1);
    CHECK(v.stats().invalid_values == 3);
    v.reset();
    CHECK(v.check(ticker(1, "100", "100.01")));
}
