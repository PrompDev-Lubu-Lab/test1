#include "tradebot/live/feed_health.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <random>

using namespace tradebot;
using namespace tradebot::live;
using namespace tradebot::market_data;
using namespace tradebot::literals;
namespace fs = std::filesystem;

TEST_CASE("FeedHealthMonitor: waiting -> healthy -> stale -> healthy") {
    FeedHealthMonitor m({.stale_after = Duration::seconds(5)});
    const Timestamp t0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");
    CHECK(m.state() == FeedState::waiting);
    CHECK_FALSE(m.check(t0));
    CHECK(m.state() == FeedState::waiting);

    Trade t;
    t.instrument = InstrumentId{1};
    t.recv_time = t0;
    t.exchange_time = t0;
    t.price = "1"_px;
    t.quantity = "1"_qty;
    m.on_trade(t);
    CHECK(m.check(t0 + Duration::seconds(1)));
    CHECK(m.state() == FeedState::healthy);
    CHECK(m.silence(t0 + Duration::seconds(1)) == Duration::seconds(1));
    CHECK(*m.last_trade() == t0);
    CHECK_FALSE(m.check(t0 + Duration::seconds(5)));  // exactly at threshold: still healthy
    CHECK(m.check(t0 + Duration::seconds(6)));
    CHECK(m.state() == FeedState::stale);
    CHECK(m.stale_episodes() == 1);
    CHECK_FALSE(m.check(t0 + Duration::seconds(7)));  // unchanged

    BookTicker bt{InstrumentId{1}, t0 + Duration::seconds(8), 1, "1"_px, "1"_qty, "2"_px, "1"_qty};
    m.on_book_ticker(bt);
    CHECK(m.check(t0 + Duration::seconds(9)));
    CHECK(m.state() == FeedState::healthy);
    CHECK(m.events() == 2);
    CHECK(*m.last_trade() == t0);  // tickers do not count as trades
    CHECK(to_string(FeedState::stale) == "stale");
}

TEST_CASE("heartbeat file: write and age") {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("tradebot-hb-" + std::to_string(rd()));
    fs::create_directories(dir);
    const fs::path file = dir / "heartbeat";
    const Timestamp t0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");
    CHECK_FALSE(heartbeat_age(file, t0).has_value());
    REQUIRE(write_heartbeat(file, t0, "healthy armed").has_value());
    auto age = heartbeat_age(file, t0 + Duration::seconds(7));
    REQUIRE(age.has_value());
    CHECK(*age == Duration::seconds(7));
    REQUIRE(write_heartbeat(file, t0 + Duration::seconds(10), "stale tripped").has_value());
    CHECK(*heartbeat_age(file, t0 + Duration::seconds(12)) == Duration::seconds(2));
    CHECK_FALSE(fs::exists(dir / "heartbeat.tmp"));
    fs::remove_all(dir);
}
