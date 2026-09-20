#include "tradebot/strategies/advanced.hpp"

#include "support/strategy_harness.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::execution;
using namespace tradebot::test;
using namespace tradebot::literals;

TEST_CASE("registry: advanced strategies are registered") {
    strategy::StrategyRegistry registry;
    strategies::register_advanced(registry);
    CHECK(registry.names() == std::vector<std::string>{"book_imbalance", "ensemble", "market_maker", "vol_trend"});
}

TEST_CASE("vol_trend: enters above the EMA with ATR-scaled size, exits on the stop") {
    // Flat, then a steady rise (enter), then a sharp drop through the stop.
    auto path = harness_concat(harness_concat(harness_ramp(3000, 0, 20), harness_ramp(3001, 1.5, 20)), harness_ramp(3030, -6, 8));
    auto r = run_strategy_path("vol_trend", "trend = 10\natr = 5\nrisk_per_trade = 0.02\natr_stop_mult = 2", path);
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[1].side == Side::sell);
    // ATR on 1.5-point candles is ~1.5-2 => stop distance ~3-4 => qty ~ 200 / 3.5 ≈ 50+, capped by cash: 9500/3001 ≈ 3.16
    CHECK(r.fills[0].fill->quantity > "1"_qty);
    CHECK(r.fills[0].fill->quantity <= "3.2"_qty);
    CHECK(r.final_position.is_zero());
    bool atr_metric = false;
    for (const auto& m : r.metrics) atr_metric |= m.name == "atr";
    CHECK(atr_metric);
}

TEST_CASE("book_imbalance: buys after confirmed bid-heavy books, sells after ask-heavy ones") {
    std::vector<MarketEvent> events;
    std::int64_t id = 1;
    auto at = [](int s) { return kHarnessT0 + Duration::seconds(s); };
    // Balanced start, then 4 bid-heavy books (imbalance +0.8), then 4 ask-heavy.
    events.push_back(harness_book(at(0), 3000, id++));
    for (int i = 1; i <= 4; ++i) events.push_back(harness_book(at(i * 2), 3000, id++, 900, 100));
    for (int i = 5; i <= 8; ++i) events.push_back(harness_book(at(i * 2), 3000, id++, 100, 900));
    events.push_back(harness_book(at(20), 3000, id++));
    auto r = run_strategy_events("book_imbalance", "levels = 5\nthreshold = 0.6\nconfirm = 3\nmin_interval = 1s\nquantity = 0.2\nmax_hold = 1h", events);
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[0].fill->quantity == "0.2"_qty);
    CHECK(r.fills[0].time == at(6));  // third consecutive bid-heavy book
    CHECK(r.fills[1].side == Side::sell);
    CHECK(r.fills[1].time == at(14));
    CHECK(r.final_position.is_zero());

    // max_hold forces an exit when the book never turns.
    std::vector<MarketEvent> hold;
    id = 1;
    hold.push_back(harness_book(at(0), 3000, id++));
    for (int i = 1; i <= 4; ++i) hold.push_back(harness_book(at(i * 2), 3000, id++, 900, 100));
    hold.push_back(harness_book(at(60), 3000, id++, 900, 100));
    auto h = run_strategy_events("book_imbalance", "confirm = 3\nmin_interval = 1s\nquantity = 0.2\nmax_hold = 20s", hold);
    // Enter at 6s, forced exit at 26s; the still bid-heavy book at 60s re-enters and the hold exits again.
    REQUIRE(h.fills.size() == 4);
    CHECK(h.fills[1].side == Side::sell);
    CHECK(h.fills[1].time == at(26));  // entered at 6s + 20s hold
    CHECK(h.fills[2].side == Side::buy);
    CHECK(h.fills[3].time == at(80));
    CHECK(h.final_position.is_zero());
}

TEST_CASE("market_maker: post-only quotes around the mid, maker fills, inventory skew and cap") {
    std::vector<MarketEvent> events;
    auto at = [](int s) { return kHarnessT0 + Duration::seconds(s); };
    events.push_back(harness_book(at(0), 3000, 1));
    // Sell-aggressor trade through our bid (2997), then buy-aggressor through our ask.
    events.push_back(harness_trade(at(5), 2996, 1, Side::sell, 5));
    events.push_back(harness_book(at(6), 3000, 2));
    events.push_back(harness_trade(at(10), 3005, 2, Side::buy, 5));
    events.push_back(harness_book(at(11), 3000, 3));
    events.push_back(harness_book(at(20), 3000, 4));
    SimulatedExchangeOptions opts;
    opts.queue_model = QueueModel::optimistic;
    auto r = run_strategy_events("market_maker", "half_spread_bps = 10\nquote_size = 0.1\nmax_inventory = 0.3\nrequote_interval = 1s\nskew_bps = 20", events, 1, opts);
    // Both sides filled once: bid at 2997 (maker), ask after the inventory was acquired.
    REQUIRE(r.fills.size() >= 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[0].fill->liquidity == Liquidity::maker);
    CHECK(r.fills[0].fill->price == "2997"_px);
    CHECK(r.fills[0].fill->quantity == "0.1"_qty);
    CHECK(r.fills[1].side == Side::sell);
    CHECK(r.fills[1].fill->liquidity == Liquidity::maker);
    // After buying, the skew lowered the ask below the unskewed 3003.
    CHECK(r.fills[1].fill->price < "3003"_px);
    CHECK(r.fills[1].fill->price > "2997"_px);
    CHECK(r.final_position.is_zero());
    CHECK(r.exchange.maker_volume >= "0.2"_qty);
    CHECK(r.exchange.taker_volume.is_zero());
    // No taker rejections for post-only quotes on a stable book.
    int rejected = 0;
    for (const auto& rep : r.reports) rejected += rep.type == ReportType::rejected;
    CHECK(rejected == 0);
    bool inv_metric = false;
    for (const auto& m : r.metrics) inv_metric |= m.name == "inventory";
    CHECK(inv_metric);
}

TEST_CASE("ensemble: needs agreeing votes to enter, exits when all signals turn off") {
    auto path = harness_concat(harness_concat(harness_ramp(3000, 0, 25), harness_ramp(3001, 2, 25)), harness_ramp(3050, -4, 25));
    auto r = run_strategy_path("ensemble", "fast = 5\nslow = 15\nbreakout = 10\nrsi = 7\nmin_votes = 2\nquantity = 0.5", path);
    REQUIRE(r.fills.size() >= 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[0].fill->price > "3000"_px);
    CHECK(r.fills.back().side == Side::sell);
    CHECK(r.final_position.is_zero());
    int max_votes = 0;
    for (const auto& m : r.metrics) if (m.name == "votes") max_votes = std::max(max_votes, static_cast<int>(m.value));
    CHECK(max_votes == 3);
}
