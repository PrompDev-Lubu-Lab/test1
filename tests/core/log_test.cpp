#include "tradebot/core/log.hpp"

#include <doctest/doctest.h>

using namespace tradebot;

TEST_CASE("Logger: writes to sink with simulated time and filters by level") {
    SimClock clock(*Timestamp::parse_iso8601("2024-01-02T03:04:05Z"));
    auto sink = std::make_shared<MemorySink>();
    Logger log = Logger::make("engine", sink, clock, LogLevel::info);

    log.debug("hidden {}", 1);
    log.info("hello {} {}", "world", 42);
    clock.advance(Duration::seconds(1));
    log.error("bad thing: {:.2f}", 3.14159);

    auto entries = sink->entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].level == LogLevel::info);
    CHECK(entries[0].component == "engine");
    CHECK(entries[0].message == "hello world 42");
    CHECK(entries[0].time.to_iso8601() == "2024-01-02T03:04:05.000000000Z");
    CHECK(entries[1].level == LogLevel::error);
    CHECK(entries[1].message == "bad thing: 3.14");
    CHECK(entries[1].time.to_iso8601() == "2024-01-02T03:04:06.000000000Z");
}

TEST_CASE("Logger: child shares config, level changes propagate") {
    SimClock clock;
    auto sink = std::make_shared<MemorySink>();
    Logger root = Logger::make("root", sink, clock, LogLevel::warn);
    Logger child = root.child("sub");
    CHECK(child.component() == "root.sub");
    child.info("nope");
    CHECK(sink->entries().empty());
    root.set_level(LogLevel::trace);
    child.trace("yes");
    REQUIRE(sink->entries().size() == 1);
    CHECK(sink->entries()[0].component == "root.sub");
    CHECK(child.enabled(LogLevel::trace));
}

TEST_CASE("Logger: default-constructed logger is a no-op") {
    Logger log;
    CHECK_FALSE(log.enabled(LogLevel::error));
    log.error("should not crash");
    log.flush();
}

TEST_CASE("parse_log_level") {
    CHECK(*parse_log_level("debug") == LogLevel::debug);
    CHECK(*parse_log_level("warning") == LogLevel::warn);
    CHECK(*parse_log_level("off") == LogLevel::off);
    CHECK_FALSE(parse_log_level("loud").has_value());
    CHECK(to_string(LogLevel::info) == "INFO");
}
