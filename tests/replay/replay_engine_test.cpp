#include "tradebot/replay/replay_engine.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::replay;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

Trade trade(Duration offset, std::uint64_t id) {
    Trade t;
    t.instrument = kEth;
    t.exchange_time = kT0 + offset;
    t.recv_time = kT0 + offset;
    t.id = TradeId{id};
    t.price = "1"_px;
    t.quantity = "1"_qty;
    t.aggressor = Side::buy;
    return t;
}

struct Recorder final : MarketDataListener {
    std::vector<std::pair<Timestamp, std::string>> log;
    const Clock* clock = nullptr;
    void on_trade(const Trade& t) override {
        log.emplace_back(clock->now(), "trade" + std::to_string(t.id.value()));
    }
    void on_book_ticker(const BookTicker& bt) override {
        log.emplace_back(clock->now(), "ticker" + std::to_string(bt.update_id));
    }
};

}  // namespace

TEST_CASE("ReplayEngine: delivers in time order and drives the clock") {
    VectorEventSource src({trade(Duration::seconds(1), 1), trade(Duration::seconds(2), 2),
                           BookTicker{kEth, kT0 + Duration::seconds(2), 9, "1"_px, "1"_qty, "2"_px, "1"_qty},
                           trade(Duration::seconds(5), 3)});
    SimClock clock(kT0);
    ZeroLatency latency;
    ReplayEngine engine(src, clock, latency);
    Recorder rec;
    rec.clock = &clock;
    engine.bus().subscribe(rec);
    int lambda_events = 0;
    engine.bus().subscribe([&](const MarketEvent&) { ++lambda_events; });

    REQUIRE(engine.run().has_value());
    REQUIRE(rec.log.size() == 4);
    CHECK(rec.log[0] == std::pair{kT0 + Duration::seconds(1), std::string("trade1")});
    CHECK(rec.log[1] == std::pair{kT0 + Duration::seconds(2), std::string("trade2")});
    CHECK(rec.log[2] == std::pair{kT0 + Duration::seconds(2), std::string("ticker9")});
    CHECK(rec.log[3] == std::pair{kT0 + Duration::seconds(5), std::string("trade3")});
    CHECK(lambda_events == 4);
    CHECK(clock.now() == kT0 + Duration::seconds(5));
    CHECK(engine.stats().events_read == 4);
    CHECK(engine.stats().events_delivered == 4);
}

TEST_CASE("ReplayEngine: latency delays delivery and can reorder relative to timers") {
    VectorEventSource src({trade(Duration::seconds(1), 1), trade(Duration::seconds(2), 2)});
    SimClock clock(kT0);
    ConstantLatency latency(Duration::millis(1500), {}, {});
    ReplayEngine engine(src, clock, latency);
    Recorder rec;
    rec.clock = &clock;
    engine.bus().subscribe(rec);
    std::vector<Timestamp> timer_fires;
    engine.schedule_at(kT0 + Duration::seconds(2), [&](Timestamp t) { timer_fires.push_back(t); });
    engine.schedule_at(kT0 + Duration::seconds(3), [&](Timestamp t) { timer_fires.push_back(t); });

    REQUIRE(engine.run().has_value());
    // trade1 delivered at 2.5s, trade2 at 3.5s; timers at 2s and 3s.
    REQUIRE(rec.log.size() == 2);
    CHECK(rec.log[0].first == kT0 + Duration::millis(2500));
    CHECK(rec.log[1].first == kT0 + Duration::millis(3500));
    REQUIRE(timer_fires.size() == 2);
    CHECK(timer_fires[0] == kT0 + Duration::seconds(2));
    CHECK(timer_fires[1] == kT0 + Duration::seconds(3));
    CHECK(clock.now() == kT0 + Duration::millis(3500));
}

TEST_CASE("ReplayEngine: random latency never delivers out of clock order") {
    std::vector<MarketEvent> events;
    for (int i = 0; i < 2000; ++i) {
        events.push_back(trade(Duration::millis(i * 7), static_cast<std::uint64_t>(i)));
    }
    VectorEventSource src(events);
    SimClock clock(kT0);
    JitterLatency latency({.market_data_base = Duration::millis(1), .market_data_jitter = Duration::millis(50)});
    ReplayEngine engine(src, clock, latency, {.seed = 42});
    Timestamp last;
    std::uint64_t delivered = 0;
    std::vector<std::uint64_t> order;
    engine.bus().subscribe([&](const MarketEvent& e) {
        CHECK(clock.now() >= last);
        CHECK(clock.now() >= event_time(e));
        last = clock.now();
        ++delivered;
        order.push_back(std::get<Trade>(e).id.value());
    });
    REQUIRE(engine.run().has_value());
    CHECK(delivered == 2000);
    // Jitter reorders some events relative to their source order.
    bool reordered = false;
    for (std::size_t i = 1; i < order.size(); ++i) {
        if (order[i] < order[i - 1]) reordered = true;
    }
    CHECK(reordered);

    // Same seed => identical delivery sequence.
    VectorEventSource src2(events);
    SimClock clock2(kT0);
    ReplayEngine engine2(src2, clock2, latency, {.seed = 42});
    std::vector<std::uint64_t> order2;
    engine2.bus().subscribe([&](const MarketEvent& e) { order2.push_back(std::get<Trade>(e).id.value()); });
    REQUIRE(engine2.run().has_value());
    CHECK(order == order2);
}

TEST_CASE("ReplayEngine: periodic timers, cancel, stop and end_time") {
    std::vector<MarketEvent> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back(trade(Duration::seconds(i * 30), static_cast<std::uint64_t>(i)));
    }
    {
        VectorEventSource src(events);
        SimClock clock(kT0 + Duration::seconds(10));
        ZeroLatency latency;
        ReplayEngine engine(src, clock, latency);
        std::vector<Timestamp> fires;
        TimerId periodic = engine.schedule_every(Duration::minutes(1), [&](Timestamp t) { fires.push_back(t); });
        TimerId cancelled = engine.schedule_at(kT0 + Duration::seconds(45), [&](Timestamp) { FAIL("cancelled timer fired"); });
        engine.cancel(cancelled);
        REQUIRE(engine.run().has_value());
        // Aligned to the minute: 10:01, 10:02, 10:03, 10:04 (last event at 10:04:30 -> source exhausted).
        REQUIRE(fires.size() >= 4);
        CHECK(fires[0] == kT0 + Duration::minutes(1));
        CHECK(fires[1] == kT0 + Duration::minutes(2));
        CHECK(fires.back() <= kT0 + Duration::minutes(5));
        CHECK(engine.stats().timers_fired == fires.size());
        engine.cancel(periodic);
    }
    {
        // stop() from a handler.
        VectorEventSource src(events);
        SimClock clock(kT0);
        ZeroLatency latency;
        ReplayEngine engine(src, clock, latency);
        int seen = 0;
        engine.bus().subscribe([&](const MarketEvent&) {
            if (++seen == 3) engine.stop();
        });
        REQUIRE(engine.run().has_value());
        CHECK(seen == 3);
        CHECK(clock.now() == kT0 + Duration::seconds(60));
    }
    {
        // end_time.
        VectorEventSource src(events);
        SimClock clock(kT0);
        ZeroLatency latency;
        ReplayEngine engine(src, clock, latency, {.end_time = kT0 + Duration::seconds(100)});
        int seen = 0;
        engine.bus().subscribe([&](const MarketEvent&) { ++seen; });
        REQUIRE(engine.run().has_value());
        CHECK(seen == 4);  // 0, 30, 60, 90
        CHECK(clock.now() == kT0 + Duration::seconds(90));
    }
    {
        // A timer scheduled in the past fires at the current time.
        VectorEventSource src({trade(Duration::seconds(5), 1)});
        SimClock clock(kT0 + Duration::seconds(3));
        ZeroLatency latency;
        ReplayEngine engine(src, clock, latency);
        Timestamp fired;
        engine.schedule_at(kT0, [&](Timestamp t) { fired = t; });
        REQUIRE(engine.run().has_value());
        CHECK(fired == kT0 + Duration::seconds(3));
    }
}
