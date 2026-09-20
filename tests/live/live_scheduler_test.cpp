#include "tradebot/live/live_scheduler.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace tradebot;
using namespace tradebot::live;
using namespace tradebot::market_data;
using namespace tradebot::literals;

namespace {

Trade trade(std::uint64_t id) {
    Trade t;
    t.instrument = InstrumentId{1};
    t.id = TradeId{id};
    t.price = "1"_px;
    t.quantity = "1"_qty;
    t.aggressor = Side::buy;
    return t;
}

}  // namespace

TEST_CASE("LiveScheduler: posted events reach both buses in order; posts run on the dispatch thread") {
    WallClock clock;
    LiveScheduler sched(clock);
    std::vector<std::uint64_t> venue_seen, client_seen;
    sched.venue_bus().subscribe([&](const MarketEvent& e) { venue_seen.push_back(std::get<Trade>(e).id.value()); });
    sched.bus().subscribe([&](const MarketEvent& e) { client_seen.push_back(std::get<Trade>(e).id.value()); });
    std::thread::id dispatch_thread;
    std::thread::id post_thread;
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= 5; ++i) sched.post_event(trade(i));
        sched.post([&] { post_thread = std::this_thread::get_id(); });
        sched.post([&] { sched.stop(); });
    });
    dispatch_thread = std::this_thread::get_id();
    sched.run();
    producer.join();
    CHECK(venue_seen == std::vector<std::uint64_t>{1, 2, 3, 4, 5});
    CHECK(client_seen == venue_seen);
    CHECK(post_thread == dispatch_thread);
    CHECK(sched.stats().events == 5);
    CHECK(sched.stats().posts == 2);
}

TEST_CASE("LiveScheduler: one-shot and periodic timers on the wall clock; cancel") {
    WallClock clock;
    LiveScheduler sched(clock);
    std::vector<Timestamp> fires;
    int periodic = 0;
    const Timestamp start = clock.now();
    sched.schedule_at(start + Duration::millis(30), [&](Timestamp t) { fires.push_back(t); });
    TimerId cancelled = sched.schedule_at(start + Duration::millis(40), [&](Timestamp) { FAIL("cancelled timer fired"); });
    sched.cancel(cancelled);
    TimerId p = sched.schedule_every(Duration::millis(25), [&](Timestamp) { ++periodic; });
    sched.schedule_at(start + Duration::millis(120), [&](Timestamp) { sched.stop(); });
    sched.run();
    REQUIRE(fires.size() == 1);
    CHECK(fires[0] >= start + Duration::millis(30));
    CHECK(fires[0] < start + Duration::millis(120));
    CHECK(periodic >= 3);
    CHECK(periodic <= 5);
    sched.cancel(p);
    CHECK(sched.stats().timers_fired == static_cast<std::uint64_t>(periodic) + 2);
}

TEST_CASE("LiveScheduler: a timer scheduled in the past fires on the next run_once") {
    WallClock clock;
    LiveScheduler sched(clock);
    bool fired = false;
    sched.schedule_at(clock.now() - Duration::seconds(10), [&](Timestamp) { fired = true; });
    sched.run_once(Duration::millis(10));
    CHECK(fired);
}
