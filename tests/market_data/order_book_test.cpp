#include "tradebot/market_data/order_book.hpp"

#include <doctest/doctest.h>

#include <map>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};

BookSnapshot snapshot(std::int64_t id, std::vector<BookLevel> bids, std::vector<BookLevel> asks) {
    BookSnapshot s;
    s.instrument = kEth;
    s.recv_time = Timestamp::from_seconds(1);
    s.last_update_id = id;
    s.bids = std::move(bids);
    s.asks = std::move(asks);
    return s;
}

BookDelta delta(std::int64_t first, std::int64_t last, std::vector<BookLevel> bids,
                std::vector<BookLevel> asks) {
    BookDelta d;
    d.instrument = kEth;
    d.exchange_time = Timestamp::from_seconds(2);
    d.recv_time = Timestamp::from_seconds(2);
    d.first_update_id = first;
    d.final_update_id = last;
    d.bids = std::move(bids);
    d.asks = std::move(asks);
    return d;
}

BookLevel lvl(const char* p, const char* q) { return {*Price::parse(p), *Quantity::parse(q)}; }

}  // namespace

TEST_CASE("OrderBook: snapshot sorts levels best-first regardless of input order") {
    OrderBook book(kEth);
    CHECK(book.empty());
    CHECK_FALSE(book.best_bid().has_value());
    CHECK_FALSE(book.mid_price().has_value());

    book.apply(snapshot(100, {lvl("99", "1"), lvl("100", "2"), lvl("98", "3")},
                        {lvl("102", "1"), lvl("101", "2"), lvl("103", "3")}));
    CHECK(book.last_update_id() == 100);
    CHECK(book.is_sorted());
    REQUIRE(book.bids().size() == 3);
    CHECK(book.bids()[0].price == "100"_px);
    CHECK(book.bids()[2].price == "98"_px);
    CHECK(book.asks()[0].price == "101"_px);
    CHECK(book.best_bid()->quantity == "2"_qty);
    CHECK(book.best_ask()->price == "101"_px);
    CHECK(*book.mid_price() == "100.5"_px);
    CHECK(*book.spread() == "1"_px);
    CHECK_FALSE(book.is_crossed());
}

TEST_CASE("OrderBook: deltas insert, update and remove levels") {
    OrderBook book(kEth);
    book.apply(snapshot(10, {lvl("100", "1"), lvl("99", "1")}, {lvl("101", "1"), lvl("102", "1")}));

    book.apply(delta(11, 12,
                     {lvl("100", "5"),  // update
                      lvl("99", "0"),  // remove
                      lvl("100.5", "2")},  // insert (new best)
                     {lvl("101", "0"),  // remove best ask
                      lvl("103", "4")}));  // insert at back
    CHECK(book.last_update_id() == 12);
    CHECK(book.is_sorted());
    REQUIRE(book.bids().size() == 2);
    CHECK(book.bids()[0].price == "100.5"_px);
    CHECK(book.bids()[1].quantity == "5"_qty);
    REQUIRE(book.asks().size() == 2);
    CHECK(book.asks()[0].price == "102"_px);
    CHECK(book.asks()[1].price == "103"_px);
    CHECK(book.quantity_at(Side::buy, "100"_px) == "5"_qty);
    CHECK(book.quantity_at(Side::buy, "99"_px).is_zero());
    CHECK(book.quantity_at(Side::sell, "103"_px) == "4"_qty);

    // Removing a level that does not exist is a no-op.
    book.apply(delta(13, 13, {lvl("50", "0")}, {}));
    CHECK(book.bids().size() == 2);

    // Crossed book detection.
    book.set_level(Side::buy, "102"_px, "1"_qty);
    CHECK(book.is_crossed());
    book.clear();
    CHECK(book.empty());
    CHECK(book.last_update_id() == 0);
}

TEST_CASE("OrderBook: liquidity and fill estimation") {
    OrderBook book(kEth);
    book.apply(snapshot(1, {lvl("100", "1"), lvl("99", "2"), lvl("98", "3")},
                        {lvl("101", "1"), lvl("102", "2"), lvl("103", "3")}));

    CHECK(book.liquidity_within(Side::sell, "102"_px) == "3"_qty);
    CHECK(book.liquidity_within(Side::buy, "99"_px) == "3"_qty);
    CHECK(book.liquidity_within(Side::buy, "100.5"_px).is_zero());

    // Market buy of 2.5: 1 @101 + 1.5 @102 = 101 + 153 = 254
    auto est = book.estimate_fill(Side::buy, "2.5"_qty);
    CHECK(est.complete("2.5"_qty));
    CHECK(est.filled == "2.5"_qty);
    CHECK(est.cost == "254"_ntl);
    CHECK(est.worst_price == "102"_px);
    CHECK(est.levels == 2);
    CHECK(est.average_price() == "101.6"_px);

    // Market sell of 10 exhausts the bids: 1@100 + 2@99 + 3@98 = 6 filled
    est = book.estimate_fill(Side::sell, "10"_qty);
    CHECK_FALSE(est.complete("10"_qty));
    CHECK(est.filled == "6"_qty);
    CHECK(est.cost == "592"_ntl);
    CHECK(est.worst_price == "98"_px);

    // Limit stops the walk.
    est = book.estimate_fill(Side::buy, "10"_qty, "102"_px);
    CHECK(est.filled == "3"_qty);
    CHECK(est.worst_price == "102"_px);
    est = book.estimate_fill(Side::sell, "10"_qty, "100"_px);
    CHECK(est.filled == "1"_qty);
    est = book.estimate_fill(Side::buy, "1"_qty, "100.5"_px);
    CHECK(est.filled.is_zero());
    CHECK(est.average_price().is_zero());

    // Imbalance: top 1 -> (1-1)/2 = 0; top 2 -> (3-3)/6 = 0; add bid.
    CHECK(book.imbalance(1) == doctest::Approx(0.0));
    book.set_level(Side::buy, "100"_px, "3"_qty);
    CHECK(book.imbalance(1) == doctest::Approx(0.5));
    OrderBook empty(kEth);
    CHECK(empty.imbalance(5) == doctest::Approx(0.0));
}

TEST_CASE("OrderBook: randomized deltas match a naive map model") {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> price_ticks(1, 200);
    std::uniform_int_distribution<int> qty_units(0, 5);  // 0 => remove
    std::uniform_int_distribution<int> side_dist(0, 1);

    OrderBook book(kEth);
    std::map<Price, Quantity> model_bids, model_asks;
    book.apply(snapshot(0, {}, {}));

    for (int step = 0; step < 5000; ++step) {
        BookDelta d = delta(step + 1, step + 1, {}, {});
        for (int i = 0; i < 3; ++i) {
            const bool bid = side_dist(rng) == 0;
            const Price p = Price::from_raw(price_ticks(rng) * 1'000'000);  // 0.01 ticks
            const Quantity q = Quantity::from_int(qty_units(rng));
            auto& model = bid ? model_bids : model_asks;
            if (q.is_zero()) {
                model.erase(p);
            } else {
                model[p] = q;
            }
            (bid ? d.bids : d.asks).push_back({p, q});
        }
        book.apply(d);
        REQUIRE(book.is_sorted());
        REQUIRE(book.bids().size() == model_bids.size());
        REQUIRE(book.asks().size() == model_asks.size());
    }
    // Compare full contents.
    std::size_t i = 0;
    for (auto it = model_bids.rbegin(); it != model_bids.rend(); ++it, ++i) {
        REQUIRE(book.bids()[i].price == it->first);
        REQUIRE(book.bids()[i].quantity == it->second);
    }
    i = 0;
    for (const auto& [p, q] : model_asks) {
        REQUIRE(book.asks()[i].price == p);
        REQUIRE(book.asks()[i].quantity == q);
        ++i;
    }
    for (const auto& [p, q] : model_bids) {
        REQUIRE(book.quantity_at(Side::buy, p) == q);
    }
}
