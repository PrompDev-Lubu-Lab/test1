#include "tradebot/execution/simulated_exchange.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::replay;
using namespace tradebot::execution;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

Instrument eth() {
    return Instrument{.id = kEth, .venue = VenueId{1}, .symbol = "ETHUSDT", .base = "ETH",
                      .quote = "USDT", .tick_size = "0.01"_px, .lot_size = "0.0001"_qty,
                      .min_quantity = "0.0001"_qty, .min_notional = "5"_ntl};
}

BookSnapshot snapshot(Duration at, std::int64_t id = 100) {
    BookSnapshot s;
    s.instrument = kEth;
    s.recv_time = kT0 + at;
    s.last_update_id = id;
    s.bids = {{"3000.00"_px, "1"_qty}, {"2999.00"_px, "2"_qty}, {"2998.00"_px, "3"_qty}};
    s.asks = {{"3001.00"_px, "1"_qty}, {"3002.00"_px, "2"_qty}, {"3003.00"_px, "3"_qty}};
    return s;
}

BookDelta delta(Duration at, std::int64_t first, std::int64_t last, std::vector<BookLevel> bids,
                std::vector<BookLevel> asks) {
    BookDelta d;
    d.instrument = kEth;
    d.exchange_time = kT0 + at;
    d.recv_time = kT0 + at;
    d.first_update_id = first;
    d.final_update_id = last;
    d.bids = std::move(bids);
    d.asks = std::move(asks);
    return d;
}

Trade trade(Duration at, const char* price, const char* qty, Side aggressor, std::uint64_t id = 1) {
    Trade t;
    t.instrument = kEth;
    t.exchange_time = kT0 + at;
    t.recv_time = kT0 + at;
    t.id = TradeId{id};
    t.price = *Price::parse(price);
    t.quantity = *Quantity::parse(qty);
    t.aggressor = aggressor;
    return t;
}

struct Reports final : ExecutionListener {
    std::vector<ExecutionReport> all;
    const Clock* clock = nullptr;
    std::vector<Timestamp> times;
    void on_execution_report(const ExecutionReport& r) override {
        all.push_back(r);
        times.push_back(clock->now());
    }
    [[nodiscard]] std::vector<ReportType> types() const {
        std::vector<ReportType> out;
        for (const auto& r : all) out.push_back(r.type);
        return out;
    }
    [[nodiscard]] std::vector<Fill> fills() const {
        std::vector<Fill> out;
        for (const auto& r : all) if (r.fill) out.push_back(*r.fill);
        return out;
    }
};

// Test harness: engine + exchange + a scripted client that acts on timers.
struct Harness {
    std::vector<MarketEvent> events;
    std::unique_ptr<VectorEventSource> source;
    SimClock clock{kT0};
    std::unique_ptr<LatencyModel> latency;
    std::unique_ptr<ReplayEngine> engine;
    std::unique_ptr<SimulatedExchange> exchange;
    Reports reports;

    explicit Harness(std::vector<MarketEvent> evs, std::unique_ptr<LatencyModel> lat = nullptr,
                     SimulatedExchangeOptions opts = SimulatedExchangeOptions{})
        : events(std::move(evs)),
          latency(lat ? std::move(lat) : std::make_unique<ZeroLatency>()) {
        source = std::make_unique<VectorEventSource>(events);
        engine = std::make_unique<ReplayEngine>(*source, clock, *latency, ReplayOptions{.seed = 7});
        exchange = std::make_unique<SimulatedExchange>(eth(), *engine, *latency, engine->rng(), opts);
        engine->venue_bus().subscribe(*exchange);
        exchange->set_listener(&reports);
        reports.clock = &clock;
    }
    void at(Duration when, std::function<void()> fn) {
        engine->schedule_at(kT0 + when, [fn = std::move(fn)](Timestamp) { fn(); });
    }
    OrderRequest limit(std::uint64_t id, Side side, const char* price, const char* qty,
                       TimeInForce tif = TimeInForce::gtc) {
        return OrderRequest{ClientOrderId{id}, kEth, StrategyId{1}, side, OrderType::limit, tif,
                            *Price::parse(price), *Quantity::parse(qty)};
    }
    OrderRequest market(std::uint64_t id, Side side, const char* qty, TimeInForce tif = TimeInForce::ioc) {
        return OrderRequest{ClientOrderId{id}, kEth, StrategyId{1}, side, OrderType::market, tif,
                            Price{}, *Quantity::parse(qty)};
    }
    void run() { REQUIRE(engine->run().has_value()); }
};

}  // namespace

TEST_CASE("SimulatedExchange: market buy walks the book, pays taker fees, consumes liquidity") {
    Harness h({snapshot(Duration::seconds(1)), trade(Duration::seconds(10), "3000", "0.1", Side::sell, 99)});
    h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.market(1, Side::buy, "2.5")).has_value()); });
    h.run();

    auto types = h.reports.types();
    REQUIRE(types.size() == 3);
    CHECK(types[0] == ReportType::accepted);
    CHECK(types[1] == ReportType::fill);
    CHECK(types[2] == ReportType::fill);
    auto fills = h.reports.fills();
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].price == "3001"_px);
    CHECK(fills[0].quantity == "1"_qty);
    CHECK(fills[0].liquidity == Liquidity::taker);
    CHECK(fills[0].fee == "3.001"_ntl);  // 0.1% of 3001
    CHECK(fills[1].price == "3002"_px);
    CHECK(fills[1].quantity == "1.5"_qty);
    CHECK(fills[1].fee == "4.503"_ntl);
    CHECK(h.reports.all.back().status == OrderStatus::filled);
    CHECK(h.reports.all.back().filled_quantity == "2.5"_qty);
    CHECK(h.reports.all.back().remaining_quantity.is_zero());
    CHECK(h.reports.times[1] == kT0 + Duration::seconds(2));  // zero latency

    // Venue book lost the consumed liquidity.
    CHECK(h.exchange->book().quantity_at(Side::sell, "3001"_px).is_zero());
    CHECK(h.exchange->book().quantity_at(Side::sell, "3002"_px) == "0.5"_qty);
    auto state = h.exchange->order(ClientOrderId{1});
    REQUIRE(state.has_value());
    CHECK(state->status == OrderStatus::filled);
    CHECK(state->filled_notional == "7504"_ntl);  // 3001 + 4503
    CHECK(state->fees == "7.504"_ntl);
    CHECK(h.exchange->stats().fills == 2);
    CHECK(h.exchange->stats().taker_volume == "2.5"_qty);
    CHECK(h.exchange->open_orders().empty());
}

TEST_CASE("SimulatedExchange: market order on thin book fills partially then expires; FOK rejects") {
    Harness h({snapshot(Duration::seconds(1))});
    h.at(Duration::seconds(2), [&] {
        REQUIRE(h.exchange->submit(h.market(1, Side::sell, "10")).has_value());
        REQUIRE(h.exchange->submit(h.market(2, Side::sell, "10", TimeInForce::fok)).has_value());
    });
    h.run();
    auto types = h.reports.types();
    // Order 1: accepted, 3 fills (1 @3000, 2 @2999, 3 @2998), expired. Order 2: rejected.
    REQUIRE(types.size() == 6);
    CHECK(types[4] == ReportType::expired);
    CHECK(h.reports.all[4].filled_quantity == "6"_qty);
    CHECK(h.reports.all[4].remaining_quantity == "4"_qty);
    CHECK(h.reports.all[4].status == OrderStatus::expired);
    CHECK(types[5] == ReportType::rejected);
    // Order 1 emptied the bid side, so the FOK finds no liquidity at all.
    CHECK(h.reports.all[5].reason.find("liquidity") != std::string::npos);
    CHECK(h.exchange->stats().expired == 1);
    CHECK(h.exchange->stats().rejected == 1);
}

TEST_CASE("SimulatedExchange: venue rules reject bad orders; duplicates rejected locally") {
    Harness h({snapshot(Duration::seconds(1))});
    h.at(Duration::seconds(2), [&] {
        REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "2999.005", "1")).has_value());  // tick
        REQUIRE(h.exchange->submit(h.limit(2, Side::buy, "2999", "0.00005")).has_value());  // lot
        REQUIRE(h.exchange->submit(h.limit(3, Side::buy, "2999", "0.001")).has_value());  // notional 2.999 < 5
        REQUIRE(h.exchange->submit(h.limit(4, Side::buy, "2999", "0.01", TimeInForce::post_only)).has_value());
        CHECK_FALSE(h.exchange->submit(h.limit(4, Side::buy, "2999", "0.01")).has_value());  // duplicate
        CHECK_FALSE(h.exchange->submit(h.limit(0, Side::buy, "2999", "0.01")).has_value());  // invalid id
        auto wrong = h.limit(5, Side::buy, "2999", "0.01");
        wrong.instrument = InstrumentId{9};
        CHECK_FALSE(h.exchange->submit(wrong).has_value());
    });
    h.run();
    auto types = h.reports.types();
    REQUIRE(types.size() == 4);
    CHECK(types[0] == ReportType::rejected);
    CHECK(types[1] == ReportType::rejected);
    CHECK(types[2] == ReportType::rejected);
    CHECK(types[3] == ReportType::accepted);
    CHECK_FALSE(h.reports.all[0].order_id.is_valid());
    CHECK(h.exchange->open_orders().size() == 1);
}

TEST_CASE("SimulatedExchange: resting limit fills by queue position against trades") {
    // Book has 1 @3000 bid. We rest a buy 0.5 @3000 behind it.
    Harness h({snapshot(Duration::seconds(1)),
               trade(Duration::seconds(3), "3000", "0.4", Side::sell, 1),   // ahead shrinks to 0.6
               trade(Duration::seconds(4), "3000.5", "5", Side::sell, 2),   // above our price: nothing
               trade(Duration::seconds(5), "3000", "0.8", Side::sell, 3),   // 0.6 ahead, 0.2 to us
               trade(Duration::seconds(6), "2999", "1", Side::sell, 4)});   // trade-through: rest fills
    h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3000", "0.5")).has_value()); });
    h.run();
    auto fills = h.reports.fills();
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].quantity == "0.2"_qty);
    CHECK(fills[0].price == "3000"_px);
    CHECK(fills[0].liquidity == Liquidity::maker);
    CHECK(fills[0].fee == "0.6"_ntl);  // 0.1% of 600
    CHECK(fills[1].quantity == "0.3"_qty);
    CHECK(fills[1].price == "3000"_px);  // filled at our limit, not the through price
    CHECK(h.reports.times[1] == kT0 + Duration::seconds(5));
    CHECK(h.reports.times[2] == kT0 + Duration::seconds(6));
    CHECK(h.reports.all.back().status == OrderStatus::filled);
    CHECK(h.exchange->stats().maker_volume == "0.5"_qty);
}

TEST_CASE("SimulatedExchange: queue models") {
    auto events = [] {
        return std::vector<MarketEvent>{snapshot(Duration::seconds(1)),
                                        trade(Duration::seconds(3), "3000", "0.5", Side::sell, 1)};
    };
    {
        Harness h(events(), nullptr, {.queue_model = QueueModel::optimistic});
        h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3000", "0.5")).has_value()); });
        h.run();
        REQUIRE(h.reports.fills().size() == 1);
        CHECK(h.reports.fills()[0].quantity == "0.5"_qty);
    }
    {
        Harness h(events(), nullptr, {.queue_model = QueueModel::queue});
        h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3000", "0.5")).has_value()); });
        h.run();
        CHECK(h.reports.fills().empty());  // 1.0 ahead, only 0.5 traded
    }
    {
        Harness h(events(), nullptr, {.queue_model = QueueModel::pessimistic});
        h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3000", "0.5")).has_value()); });
        h.run();
        CHECK(h.reports.fills().empty());
    }
}

TEST_CASE("SimulatedExchange: book reductions shrink queue ahead; crossing book fills resting order") {
    Harness h({snapshot(Duration::seconds(1)),
               delta(Duration::seconds(3), 101, 101, {{"3000"_px, "0.2"_qty}}, {}),  // cancels ahead of us
               trade(Duration::seconds(4), "3000", "0.3", Side::sell, 1),  // 0.2 ahead, 0.1 to us
               delta(Duration::seconds(5), 102, 102, {}, {{"3000"_px, "1"_qty}, {"3001"_px, "0"_qty}})});  // ask crosses
    h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3000", "0.5")).has_value()); });
    h.run();
    auto fills = h.reports.fills();
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].quantity == "0.1"_qty);
    CHECK(fills[1].quantity == "0.4"_qty);
    CHECK(h.reports.times[2] == kT0 + Duration::seconds(5));
}

TEST_CASE("SimulatedExchange: marketable limit takes then rests; IOC expires remainder; post-only rejects") {
    Harness h({snapshot(Duration::seconds(1))});
    h.at(Duration::seconds(2), [&] {
        REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3001.5", "1.5")).has_value());  // takes 1 @3001, rests 0.5
        REQUIRE(h.exchange->submit(h.limit(2, Side::sell, "2999", "5", TimeInForce::ioc)).has_value());  // 1@3000 + 2@2999, expire 2
        REQUIRE(h.exchange->submit(h.limit(3, Side::buy, "3002", "0.1", TimeInForce::post_only)).has_value());  // crosses ask 3002
        REQUIRE(h.exchange->submit(h.limit(4, Side::buy, "3001.5", "0.1", TimeInForce::fok)).has_value());  // nothing left <= 3001.5
    });
    h.run();
    auto o1 = h.exchange->order(ClientOrderId{1});
    REQUIRE(o1.has_value());
    CHECK(o1->status == OrderStatus::partially_filled);
    CHECK(o1->filled_quantity == "1"_qty);
    CHECK(o1->remaining() == "0.5"_qty);
    auto o2 = h.exchange->order(ClientOrderId{2});
    CHECK(o2->status == OrderStatus::expired);
    CHECK(o2->filled_quantity == "3"_qty);
    auto o3 = h.exchange->order(ClientOrderId{3});
    CHECK(o3->status == OrderStatus::rejected);
    auto o4 = h.exchange->order(ClientOrderId{4});
    CHECK(o4->status == OrderStatus::rejected);
    CHECK(h.exchange->open_orders().size() == 1);
    CHECK(h.exchange->open_orders()[0].request.client_id == ClientOrderId{1});
}

TEST_CASE("SimulatedExchange: cancel semantics") {
    Harness h({snapshot(Duration::seconds(1)), trade(Duration::seconds(4), "2990", "5", Side::sell, 1)});
    h.at(Duration::seconds(2), [&] {
        REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "2999", "0.5")).has_value());
        REQUIRE(h.exchange->submit(h.limit(2, Side::buy, "2998", "0.5")).has_value());
        CHECK_FALSE(h.exchange->cancel(ClientOrderId{99}).has_value());  // unknown
    });
    h.at(Duration::seconds(3), [&] { REQUIRE(h.exchange->cancel(ClientOrderId{1}).has_value()); });
    h.at(Duration::seconds(5), [&] {
        // Order 2 filled at t=4 by the trade-through; cancel now is rejected.
        REQUIRE_FALSE(h.exchange->cancel(ClientOrderId{2}).has_value());
    });
    h.run();
    auto o1 = h.exchange->order(ClientOrderId{1});
    CHECK(o1->status == OrderStatus::cancelled);
    CHECK(o1->filled_quantity.is_zero());
    auto o2 = h.exchange->order(ClientOrderId{2});
    CHECK(o2->status == OrderStatus::filled);
    auto types = h.reports.types();
    // accepted(1), accepted(2), cancelled(1), fill(2)
    REQUIRE(types.size() == 4);
    CHECK(types[2] == ReportType::cancelled);
    CHECK(types[3] == ReportType::fill);
    CHECK(h.exchange->stats().cancelled == 1);
    CHECK(h.exchange->open_orders().empty());
}

TEST_CASE("SimulatedExchange: latency delays arrival and reports; cancel racing the order") {
    Harness h({snapshot(Duration::seconds(1)),
               // At t=2.05 the book moves: ask 3001 gone. An order sent at t=2 with 100ms
               // order latency arrives at 2.1 and sees the new book.
               delta(Duration::millis(2050), 101, 101, {}, {{"3001"_px, "0"_qty}})},
              std::make_unique<ConstantLatency>(Duration{}, Duration::millis(100), Duration::millis(50)));
    h.at(Duration::seconds(2), [&] { REQUIRE(h.exchange->submit(h.market(1, Side::buy, "1")).has_value()); });
    h.at(Duration::seconds(3), [&] {
        REQUIRE(h.exchange->submit(h.limit(2, Side::buy, "2999", "0.5")).has_value());
        REQUIRE(h.exchange->cancel(ClientOrderId{2}).has_value());  // arrives together with the order
    });
    h.run();
    auto fills = h.reports.fills();
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].price == "3002"_px);  // 3001 was gone by arrival
    CHECK(h.reports.times[0] == kT0 + Duration::millis(2150));  // accepted: 2.1 arrival + 50ms ack
    CHECK(h.reports.all[0].time == kT0 + Duration::millis(2100));
    // Order 2 arrived at 3.1, cancel at 3.1 (scheduled after): cancelled.
    auto o2 = h.exchange->order(ClientOrderId{2});
    CHECK(o2->status == OrderStatus::cancelled);
}

TEST_CASE("SimulatedExchange: aggressive orders are rejected while the book is not synchronized") {
    Harness h({snapshot(Duration::seconds(1)),
               delta(Duration::seconds(2), 150, 151, {}, {})});  // gap => resync required
    h.at(Duration::seconds(3), [&] {
        REQUIRE(h.exchange->submit(h.market(1, Side::buy, "1")).has_value());
        REQUIRE(h.exchange->submit(h.limit(2, Side::buy, "2999", "0.5")).has_value());  // resting is fine
    });
    h.run();
    CHECK(h.exchange->order(ClientOrderId{1})->status == OrderStatus::rejected);
    CHECK(h.exchange->order(ClientOrderId{2})->status == OrderStatus::open);
}

TEST_CASE("SimulatedExchange: determinism with jitter latency") {
    auto run = [] {
        Harness h({snapshot(Duration::seconds(1)), trade(Duration::seconds(3), "2999", "5", Side::sell, 1),
                   trade(Duration::seconds(4), "3003", "5", Side::buy, 2)},
                  std::make_unique<JitterLatency>(JitterLatency::Params{}));
        h.at(Duration::seconds(2), [&] {
            REQUIRE(h.exchange->submit(h.limit(1, Side::buy, "3000", "0.5")).has_value());
            REQUIRE(h.exchange->submit(h.limit(2, Side::sell, "3002", "0.5")).has_value());
        });
        h.run();
        return h.reports.times;
    };
    auto a = run();
    auto b = run();
    REQUIRE(a.size() == 4);
    CHECK(a == b);
}

TEST_CASE("SimulatedExchange: trades-only data fills at the last trade with slippage") {
    // No book at all: only trades. Market buy fills at last trade * (1 + 5bps), rounded up to tick.
    Harness h({trade(Duration::seconds(1), "3000", "1", Side::buy, 1),
               trade(Duration::seconds(5), "2990", "1", Side::sell, 2)});
    h.at(Duration::seconds(2), [&] {
        REQUIRE(h.exchange->submit(h.market(1, Side::buy, "0.5")).has_value());
        REQUIRE(h.exchange->submit(h.limit(2, Side::buy, "3000.5", "0.5")).has_value());  // marketable: capped at limit
        REQUIRE(h.exchange->submit(h.limit(3, Side::buy, "2995", "0.5")).has_value());  // rests, fills on the 2990 trade
        REQUIRE(h.exchange->submit(h.limit(4, Side::sell, "3010", "0.5", TimeInForce::post_only)).has_value());
    });
    h.run();
    auto fills = h.reports.fills();
    REQUIRE(fills.size() == 3);
    CHECK(fills[0].price == "3001.5"_px);  // 3000 * 1.0005
    CHECK(fills[0].quantity == "0.5"_qty);
    CHECK(fills[0].liquidity == Liquidity::taker);
    CHECK(fills[1].price == "3000.5"_px);  // limit caps the slipped price
    CHECK(fills[2].price == "2995"_px);  // resting buy filled by trade-through at its own price
    CHECK(fills[2].liquidity == Liquidity::maker);
    CHECK(h.exchange->order(ClientOrderId{4})->status == OrderStatus::open);  // resting ask untouched
    CHECK(*h.exchange->last_trade_price() == "2990"_px);

    // Fallback disabled: nothing fills without a book.
    Harness strict({trade(Duration::seconds(1), "3000", "1", Side::buy, 1)}, nullptr, {.fallback_to_trades = false});
    strict.at(Duration::seconds(2), [&] { REQUIRE(strict.exchange->submit(strict.market(1, Side::buy, "0.5")).has_value()); });
    strict.run();
    CHECK(strict.exchange->order(ClientOrderId{1})->status == OrderStatus::rejected);
}
