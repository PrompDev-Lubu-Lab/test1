#include "tradebot/backtest/backtest.hpp"

#include "tradebot/strategies/baselines.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::backtest;
using namespace tradebot::storage;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-bt-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const InstrumentId kEth{1};
const Timestamp kDay = *Timestamp::parse_iso8601("2024-03-15");

// Writes two days of synthetic data: a book snapshot every minute and a
// trade every minute following a sine wave with a 12-hour period, so
// moving-average crossovers happen several times.
void write_store(const StorePath& store) {
    EventStoreWriter w(store, kEth, DataSource::bulk);
    std::uint64_t id = 1;
    const int minutes = 2 * 24 * 60;
    for (int i = 0; i < minutes; ++i) {
        const double mid = 3000.0 + 100.0 * std::sin(2.0 * 3.14159265358979 * i / 720.0);
        const Timestamp t = kDay + Duration::minutes(i);
        BookSnapshot s;
        s.instrument = kEth;
        s.recv_time = t;
        s.last_update_id = 1000 + i;
        s.bids = {{Price::from_double(mid - 0.5).round_to("0.01"_px, RoundingMode::down), "500"_qty}};
        s.asks = {{Price::from_double(mid + 0.5).round_to("0.01"_px, RoundingMode::up), "500"_qty}};
        REQUIRE(w.write(s).has_value());
        Trade tr;
        tr.instrument = kEth;
        tr.exchange_time = t + Duration::seconds(1);
        tr.recv_time = tr.exchange_time;
        tr.id = TradeId{id++};
        tr.price = Price::from_double(mid).round_to("0.01"_px, RoundingMode::nearest);
        tr.quantity = "1"_qty;
        tr.aggressor = Side::buy;
        REQUIRE(w.write(tr).has_value());
    }
    REQUIRE(w.close().has_value());
}

std::string config_text(const fs::path& data_dir, const std::string& extra = {}) {
    return "[backtest]\nsymbol = ETHUSDT\nfrom = 2024-03-15\nto = 2024-03-17\ninitial_cash = 10000\nseed = 5\n"
           "sample_interval = 1h\n[data]\ndir = " + data_dir.string() +
           "\n[exchange]\nlatency = constant\nlatency_market_data = 10ms\nlatency_order = 20ms\nlatency_ack = 5ms\n"
           "[risk]\nmax_position = 10\nmax_price_deviation = 0\n"
           "[strategy]\nname = ma_crossover\nlabel = ma\n[strategy.params]\ninterval = 15m\nfast = 4\nslow = 12\nquantity = 1\n" + extra;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("parse_backtest_spec: defaults, sections, errors") {
    TempDir tmp;
    auto cfg = Config::parse(config_text(tmp.path));
    REQUIRE(cfg.has_value());
    auto spec = parse_backtest_spec(*cfg);
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    CHECK(spec->store.symbol == "ETHUSDT");
    CHECK(spec->store.venue == "binance");
    CHECK(spec->from == kDay);
    CHECK(spec->initial_cash == "10000"_ntl);
    CHECK(spec->seed == 5);
    CHECK(spec->sample_interval == Duration::hours(1));
    CHECK(spec->instrument.tick_size == "0.01"_px);
    CHECK(spec->instrument.min_notional == "5"_ntl);
    CHECK(spec->exchange.fees.taker.numerator == 10);
    CHECK(spec->exchange.queue_model == execution::QueueModel::queue);
    CHECK(spec->latency.kind == LatencySpec::Kind::constant);
    CHECK(spec->latency.order == Duration::millis(20));
    CHECK(spec->limits.max_position == "10"_qty);
    CHECK(spec->limits.max_price_deviation == doctest::Approx(0.0));
    REQUIRE(spec->strategies.size() == 1);
    CHECK(spec->strategies[0].name == "ma_crossover");
    CHECK(spec->strategies[0].label == "ma");
    CHECK(*spec->strategies[0].params.get_int("fast") == 4);
    CHECK(spec->run_id.empty());
    const std::string id = spec->derived_run_id();
    CHECK(id.find("ma_2024-03-15_2024-03-17_") == 0);
    CHECK(id.size() == std::string("ma_2024-03-15_2024-03-17_").size() + 8);
    CHECK(spec->describe().find("fast = 4") != std::string::npos);

    auto bad = Config::parse("[backtest]\nfrom = 2024-03-15\nto = 2024-03-10\ninitial_cash = 1\n[strategy]\nname = x");
    CHECK_FALSE(parse_backtest_spec(*bad).has_value());
    auto missing = Config::parse("[backtest]\nfrom = 2024-03-15\nto = 2024-03-17\n[strategy]\nname = x");
    CHECK_FALSE(parse_backtest_spec(*missing).has_value());  // no initial_cash
    auto badq = Config::parse(config_text(tmp.path, "[exchange]\nqueue_model = maybe\n"));
    CHECK_FALSE(parse_backtest_spec(*badq).has_value());
}

TEST_CASE("expand_sweep: cartesian product with labels") {
    TempDir tmp;
    auto spec = *parse_backtest_spec(*Config::parse(config_text(tmp.path)));
    auto sweep = *Config::parse("fast = 3, 5\nslow = 10, 20, 30");
    auto runs = expand_sweep(spec, sweep);
    REQUIRE_MESSAGE(runs.has_value(), runs.error().message);
    REQUIRE(runs->size() == 6);
    CHECK(*(*runs)[0].strategies[0].params.get_int("fast") == 3);
    CHECK(*(*runs)[0].strategies[0].params.get_int("slow") == 10);
    CHECK(*(*runs)[1].strategies[0].params.get_int("fast") == 5);
    CHECK(*(*runs)[1].strategies[0].params.get_int("slow") == 10);
    CHECK(*(*runs)[5].strategies[0].params.get_int("slow") == 30);
    CHECK((*runs)[5].strategies[0].label == "ma_fast=5_slow=30");
    CHECK(*(*runs)[5].strategies[0].params.get_string("interval") == "15m");  // other keys preserved
    std::set<std::string> ids;
    for (const auto& r : *runs) ids.insert(r.derived_run_id());
    CHECK(ids.size() == 6);
    CHECK_FALSE(expand_sweep(spec, *Config::parse("fast = ")).has_value());
}

TEST_CASE("run_backtest: end to end on a synthetic store, artifacts, determinism") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    write_store(store);
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);

    auto spec = *parse_backtest_spec(*Config::parse(config_text(tmp.path)));
    auto result = run_backtest(spec, registry, Logger{});
    REQUIRE_MESSAGE(result.has_value(), result.error().message);
    const auto& s = result->summary;
    CHECK(result->spec.run_id == spec.derived_run_id());
    CHECK(s.events == 2 * 2 * 24 * 60);
    CHECK(s.fills >= 2);  // the price path produces at least one round trip
    CHECK(s.orders == s.fills);  // market orders fill in one piece on this deep book
    CHECK(s.rejected == 0);
    CHECK(s.initial_cash == "10000"_ntl);
    CHECK(s.final_equity != s.initial_cash);
    CHECK(s.fees.is_positive());
    CHECK(s.max_drawdown_fraction >= 0.0);
    CHECK(s.wall_time.count_nanos() > 0);
    // Hourly samples over 2 days plus start and end.
    CHECK(result->equity_curve.size() >= 48);
    CHECK(result->equity_curve.front().time == kDay);
    CHECK(result->fills.size() == s.fills);
    CHECK(result->orders.size() >= 2 * s.fills);  // accepted + fill per order
    REQUIRE(result->strategy_labels.size() == 1);
    CHECK(result->strategy_labels[0].second == "ma");
    CHECK_FALSE(result->metrics.empty());

    // Artifacts.
    const fs::path out = tmp.path / "runs" / result->spec.run_id;
    REQUIRE(write_artifacts(*result, out).has_value());
    for (const char* f : {"config.txt", "equity.csv", "fills.csv", "orders.csv", "metrics.csv", "summary.json"}) {
        CHECK_MESSAGE(fs::exists(out / f), f);
    }
    const std::string equity = read_file(out / "equity.csv");
    CHECK(equity.find("time,equity,cash") == 0);
    CHECK(std::count(equity.begin(), equity.end(), '\n') == static_cast<long>(result->equity_curve.size() + 1));
    const std::string summary = read_file(out / "summary.json");
    CHECK(summary.find("\"final_equity\"") != std::string::npos);
    CHECK(summary.find("\"fills\": " + std::to_string(s.fills)) != std::string::npos);
    CHECK(read_file(out / "config.txt").find("run_id = " + result->spec.run_id) == 0);

    // Same spec, same seed => identical equity curve and fills.
    auto again = run_backtest(spec, registry, Logger{});
    REQUIRE(again.has_value());
    REQUIRE(again->equity_curve.size() == result->equity_curve.size());
    for (std::size_t i = 0; i < again->equity_curve.size(); ++i) {
        CHECK(again->equity_curve[i].equity == result->equity_curve[i].equity);
    }
    REQUIRE(again->fills.size() == result->fills.size());
    for (std::size_t i = 0; i < again->fills.size(); ++i) {
        CHECK(again->fills[i].time == result->fills[i].time);
        CHECK(again->fills[i].price == result->fills[i].price);
    }

    // A different latency draw (jitter, other seed) changes fill times but not the fill count.
    auto spec2 = spec;
    spec2.seed = 99;
    spec2.latency.kind = LatencySpec::Kind::jitter;
    auto other = run_backtest(spec2, registry, Logger{});
    REQUIRE(other.has_value());
    CHECK(other->spec.run_id != result->spec.run_id);
}

TEST_CASE("run_backtest: unknown strategy and empty store") {
    TempDir tmp;
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto spec = *parse_backtest_spec(*Config::parse(config_text(tmp.path)));
    spec.strategies[0].name = "nope";
    auto r = run_backtest(spec, registry, Logger{});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::not_found);

    spec.strategies[0].name = "buy_and_hold";
    auto empty = run_backtest(spec, registry, Logger{});  // no data: runs to nothing
    REQUIRE(empty.has_value());
    CHECK(empty->summary.events == 0);
    CHECK(empty->summary.fills == 0);
    CHECK(empty->summary.final_equity == "10000"_ntl);
}

TEST_CASE("run_batch: parallel runs keep order and isolate failures") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    write_store(store);
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto base = *parse_backtest_spec(*Config::parse(config_text(tmp.path)));
    auto specs = *expand_sweep(base, *Config::parse("fast = 3, 5\nslow = 10, 20"));
    specs[2].strategies[0].name = "missing";
    auto results = run_batch(specs, registry, Logger{}, 3);
    REQUIRE(results.size() == 4);
    CHECK(results[0].has_value());
    CHECK(results[1].has_value());
    CHECK_FALSE(results[2].has_value());
    CHECK(results[3].has_value());
    CHECK(results[0]->spec.strategies[0].label == "ma_fast=3_slow=10");
    CHECK(results[3]->spec.strategies[0].label == "ma_fast=5_slow=20");
    // Batch results equal single runs of the same spec.
    auto single = run_backtest(specs[3], registry, Logger{});
    REQUIRE(single.has_value());
    CHECK(single->summary.final_equity == results[3]->summary.final_equity);
}
