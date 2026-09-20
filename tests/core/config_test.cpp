#include "tradebot/core/config.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <cstdlib>

using namespace tradebot;
using namespace tradebot::literals;

namespace {
constexpr std::string_view kSample = R"(
# top-level comment
name = tradebot
verbose = true

[exchange]
venue = "binance"   # quoted values keep everything inside
api_key = "abc#123"
timeout = 2500ms
fee_rate = 0.001
max_orders = 20

[backtest]
start = 2024-01-01T00:00:00Z
tick = 0.01
size = 0.5
capital = 10000
)";
}  // namespace

TEST_CASE("Config: parse sections, comments, quotes") {
    auto cfg = Config::parse(kSample);
    REQUIRE_MESSAGE(cfg.has_value(), cfg.error().message);
    CHECK(*cfg->get_string("name") == "tradebot");
    CHECK(*cfg->get_bool("verbose") == true);
    CHECK(*cfg->get_string("exchange.venue") == "binance");
    CHECK(*cfg->get_string("exchange.api_key") == "abc#123");
    CHECK(*cfg->get_duration("exchange.timeout") == Duration::millis(2500));
    CHECK(*cfg->get_double("exchange.fee_rate") == doctest::Approx(0.001));
    CHECK(*cfg->get_int("exchange.max_orders") == 20);
    CHECK(*cfg->get_timestamp("backtest.start") == *Timestamp::parse_iso8601("2024-01-01"));
    CHECK(*cfg->get_price("backtest.tick") == "0.01"_px);
    CHECK(*cfg->get_quantity("backtest.size") == "0.5"_qty);
    CHECK(*cfg->get_notional("backtest.capital") == "10000"_ntl);
}

TEST_CASE("Config: errors name the key") {
    auto cfg = *Config::parse(kSample);
    auto missing = cfg.get_int("exchange.nope");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::not_found);
    CHECK(missing.error().message.find("exchange.nope") != std::string::npos);

    auto bad = cfg.get_int("exchange.venue");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::parse_error);
    CHECK(bad.error().message.find("exchange.venue") != std::string::npos);

    CHECK_FALSE(cfg.get_bool("name").has_value());
    CHECK_FALSE(cfg.get_duration("name").has_value());
}

TEST_CASE("Config: defaults only apply to missing keys") {
    auto cfg = *Config::parse(kSample);
    CHECK(*cfg.get_int_or("exchange.max_orders", 5) == 20);
    CHECK(*cfg.get_int_or("exchange.missing", 5) == 5);
    CHECK_FALSE(cfg.get_int_or("exchange.venue", 5).has_value());
    CHECK(*cfg.get_duration_or("x", Duration::seconds(3)) == Duration::seconds(3));
    CHECK(*cfg.get_bool_or("x", true) == true);
    CHECK(*cfg.get_string_or("x", "d") == "d");
    CHECK(*cfg.get_double_or("x", 1.5) == doctest::Approx(1.5));
}

TEST_CASE("Config: malformed input") {
    CHECK_FALSE(Config::parse("[unterminated").has_value());
    CHECK_FALSE(Config::parse("[]").has_value());
    CHECK_FALSE(Config::parse("no equals sign").has_value());
    CHECK_FALSE(Config::parse("= value").has_value());
    CHECK_FALSE(Config::parse("k = \"open").has_value());
    CHECK_FALSE(Config::parse("k = \"a\" b").has_value());
    CHECK(*Config::parse("k = \"a\" ; c")->get_string("k") == "a");
    CHECK(*Config::parse("k = \"\"")->get_string("k") == "");
    CHECK(Config::parse("").has_value());
    CHECK(Config::parse("key=").has_value());
    CHECK(*Config::parse("key=")->get_string("key") == "");
}

TEST_CASE("Config: section view and set") {
    auto cfg = *Config::parse(kSample);
    Config ex = cfg.section("exchange");
    CHECK(*ex.get_string("venue") == "binance");
    CHECK_FALSE(ex.contains("name"));
    CHECK(ex.keys().size() == 5);
    ex.set("venue", "kraken");
    CHECK(*ex.get_string("venue") == "kraken");
    CHECK(*cfg.get_string("exchange.venue") == "binance");  // original untouched
}

TEST_CASE("Config: environment overrides") {
    setenv("TRADEBOT_EXCHANGE_API_KEY", "from-env", 1);
    setenv("TRADEBOT_TOPLEVEL", "7", 1);
    auto cfg = *Config::parse(kSample);
    cfg.apply_env_overrides();
    CHECK(*cfg.get_string("exchange.api_key") == "from-env");
    CHECK(*cfg.get_int("toplevel") == 7);
    CHECK(*cfg.get_string("exchange.venue") == "binance");
    unsetenv("TRADEBOT_EXCHANGE_API_KEY");
    unsetenv("TRADEBOT_TOPLEVEL");
}

TEST_CASE("parse_duration") {
    CHECK(*parse_duration("10ns") == Duration::nanos(10));
    CHECK(*parse_duration("250us") == Duration::micros(250));
    CHECK(*parse_duration("5 ms") == Duration::millis(5));
    CHECK(*parse_duration("2s") == Duration::seconds(2));
    CHECK(*parse_duration("5m") == Duration::minutes(5));
    CHECK(*parse_duration("1h") == Duration::hours(1));
    CHECK(*parse_duration("3d") == Duration::days(3));
    CHECK_FALSE(parse_duration("5").has_value());
    CHECK_FALSE(parse_duration("ms").has_value());
    CHECK_FALSE(parse_duration("5 weeks").has_value());
    CHECK_FALSE(parse_duration("").has_value());
}

TEST_CASE("Config: layered files merge key by key, later layers win") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tradebot-config-layers";
    fs::create_directories(dir);
    {
        std::ofstream(dir / "base.conf") << "[risk]\nmax_position = 1\nmax_drawdown = 100\n[live]\nmode = paper\n";
        std::ofstream(dir / "env.conf") << "[risk]\nmax_position = 0.05\n[live]\nmode = shadow\nrecv_window = 5s\n";
    }
    auto cfg = Config::load_files({(dir / "base.conf").string(), (dir / "env.conf").string()});
    REQUIRE_MESSAGE(cfg.has_value(), cfg.error().message);
    CHECK(*cfg->get_string("risk.max_position") == "0.05");  // overridden
    CHECK(*cfg->get_string("risk.max_drawdown") == "100");  // kept from base
    CHECK(*cfg->get_string("live.mode") == "shadow");
    CHECK(*cfg->get_string("live.recv_window") == "5s");  // added by the layer
    CHECK_FALSE(Config::load_files({}).has_value());
    auto missing = Config::load_files({(dir / "base.conf").string(), (dir / "nope.conf").string()});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message.find("nope.conf") != std::string::npos);
    fs::remove_all(dir);
}
