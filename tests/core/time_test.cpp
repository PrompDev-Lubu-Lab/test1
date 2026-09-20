#include "tradebot/core/time.hpp"

#include <doctest/doctest.h>

using namespace tradebot;

TEST_CASE("Duration: constructors and conversions") {
    CHECK(Duration::seconds(1).count_nanos() == 1'000'000'000);
    CHECK(Duration::millis(1500).count_seconds() == 1);
    CHECK(Duration::minutes(2) == Duration::seconds(120));
    CHECK(Duration::hours(1) == Duration::minutes(60));
    CHECK(Duration::days(1) == Duration::hours(24));
    CHECK(Duration::from_chrono(std::chrono::milliseconds(3)) == Duration::millis(3));
    CHECK(Duration::millis(3).to_chrono() == std::chrono::milliseconds(3));
    CHECK(Duration::millis(1500).as_seconds() == doctest::Approx(1.5));
    CHECK(Duration::hours(1) / Duration::minutes(15) == 4);
    CHECK((Duration::seconds(1) * 3) == Duration::seconds(3));
    CHECK((-Duration::seconds(1)).is_negative());
}

TEST_CASE("Duration: to_string picks a sensible unit") {
    CHECK(Duration::nanos(250).to_string() == "250ns");
    CHECK(Duration::micros(3).to_string() == "3.000us");
    CHECK(Duration::millis(12).to_string() == "12.000ms");
    CHECK(Duration::seconds(2).to_string() == "2.000s");
    CHECK(Duration::seconds(90).to_string() == "1.50m");
    CHECK(Duration::hours(1).to_string() == "1.00h");
    CHECK(Duration::days(3).to_string() == "3.00d");
    CHECK((-Duration::millis(5)).to_string() == "-5.000ms");
}

TEST_CASE("Timestamp: ISO-8601 parse and format") {
    auto t = Timestamp::parse_iso8601("2024-03-15T12:34:56.123456789Z");
    REQUIRE(t.has_value());
    CHECK(t->to_iso8601() == "2024-03-15T12:34:56.123456789Z");
    CHECK(t->seconds_since_epoch() == 1710506096);

    CHECK(Timestamp::parse_iso8601("1970-01-01T00:00:00Z")->nanos_since_epoch() == 0);
    CHECK(Timestamp::parse_iso8601("1970-01-01")->nanos_since_epoch() == 0);
    CHECK(Timestamp::parse_iso8601("2000-02-29")->seconds_since_epoch() == 951782400);
    CHECK(Timestamp::parse_iso8601("2024-01-01T00:00:00.5Z")->nanos_since_epoch() ==
          1704067200 * 1'000'000'000LL + 500'000'000);
    // Truncates beyond nanoseconds
    CHECK(Timestamp::parse_iso8601("1970-01-01T00:00:00.0000000019Z")->nanos_since_epoch() == 1);

    CHECK(Timestamp::epoch().to_iso8601() == "1970-01-01T00:00:00.000000000Z");
    CHECK(Timestamp::from_nanos(-1).to_iso8601() == "1969-12-31T23:59:59.999999999Z");
    CHECK(Timestamp::from_seconds(-86400).to_iso8601() == "1969-12-31T00:00:00.000000000Z");
}

TEST_CASE("Timestamp: parse rejects non-UTC and malformed input") {
    CHECK_FALSE(Timestamp::parse_iso8601("").has_value());
    CHECK_FALSE(Timestamp::parse_iso8601("2024-03-15T12:34:56").has_value());  // no Z
    CHECK_FALSE(Timestamp::parse_iso8601("2024-03-15T12:34:56+01:00").has_value());
    CHECK_FALSE(Timestamp::parse_iso8601("2024-13-01").has_value());
    CHECK_FALSE(Timestamp::parse_iso8601("2024-03-15T25:00:00Z").has_value());
    CHECK_FALSE(Timestamp::parse_iso8601("2024-03-15T12:34:56.Z").has_value());
    CHECK_FALSE(Timestamp::parse_iso8601("2024-3-15").has_value());
    CHECK_FALSE(Timestamp::parse_iso8601("2024-03-15 12:34:56Z").has_value());
}

TEST_CASE("Timestamp: arithmetic and bucketing") {
    const auto t = *Timestamp::parse_iso8601("2024-03-15T12:34:56.789Z");
    CHECK((t + Duration::seconds(4)).to_iso8601() == "2024-03-15T12:35:00.789000000Z");
    CHECK((t - Duration::hours(13)).to_iso8601() == "2024-03-14T23:34:56.789000000Z");
    CHECK((t + Duration::seconds(4)) - t == Duration::seconds(4));
    CHECK(t.floor_to(Duration::minutes(1)).to_iso8601() == "2024-03-15T12:34:00.000000000Z");
    CHECK(t.floor_to(Duration::hours(1)).to_iso8601() == "2024-03-15T12:00:00.000000000Z");
    CHECK(t.floor_to(Duration::days(1)).to_iso8601() == "2024-03-15T00:00:00.000000000Z");
    CHECK(Timestamp::from_nanos(-1).floor_to(Duration::seconds(1)) ==
          Timestamp::from_seconds(-1));
    CHECK(t < t + Duration::nanos(1));

    const auto tp = t.to_chrono();
    CHECK(Timestamp::from_chrono(tp) == t);
}
