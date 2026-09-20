#include "tradebot/core/clock.hpp"

#include <doctest/doctest.h>

#include <stdexcept>

using namespace tradebot;

TEST_CASE("SimClock: moves forward only") {
    SimClock clock(Timestamp::from_seconds(100));
    CHECK(clock.now() == Timestamp::from_seconds(100));
    clock.advance(Duration::seconds(5));
    CHECK(clock.now() == Timestamp::from_seconds(105));
    clock.set(Timestamp::from_seconds(105));  // same time is fine
    clock.set(Timestamp::from_seconds(200));
    CHECK(clock.now() == Timestamp::from_seconds(200));
    CHECK_THROWS_AS(clock.set(Timestamp::from_seconds(199)), std::logic_error);
    CHECK_THROWS_AS(clock.advance(Duration::seconds(-1)), std::logic_error);
    CHECK(clock.now() == Timestamp::from_seconds(200));
}

TEST_CASE("WallClock: is roughly now and monotone across two reads") {
    WallClock clock;
    const Timestamp a = clock.now();
    const Timestamp b = clock.now();
    CHECK(b >= a);
    // Sanity: after 2020 and before 2100.
    CHECK(a > *Timestamp::parse_iso8601("2020-01-01"));
    CHECK(a < *Timestamp::parse_iso8601("2100-01-01"));
}

TEST_CASE("Clock: polymorphic use") {
    SimClock sim(Timestamp::from_seconds(7));
    const Clock& clock = sim;
    CHECK(clock.now() == Timestamp::from_seconds(7));
}
