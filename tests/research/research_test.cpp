#include "tradebot/research/research.hpp"

#include "tradebot/strategies/baselines.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::research;
using namespace tradebot::storage;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-rs-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const InstrumentId kEth{1};
const Timestamp kDay = *Timestamp::parse_iso8601("2024-03-01");

// Eight days of one-minute trades on a sine wave (12h period): enough for
// several walk-forward windows.
void write_store(const StorePath& store, int days = 8) {
    EventStoreWriter w(store, kEth, DataSource::bulk);
    std::uint64_t id = 1;
    for (int i = 0; i < days * 24 * 60; ++i) {
        const double mid = 3000.0 + 100.0 * std::sin(2.0 * 3.14159265358979 * i / 720.0);
        Trade tr;
        tr.instrument = kEth;
        tr.exchange_time = kDay + Duration::minutes(i);
        tr.recv_time = tr.exchange_time;
        tr.id = TradeId{id++};
        tr.price = Price::from_double(mid).round_to("0.01"_px, RoundingMode::nearest);
        tr.quantity = "1"_qty;
        tr.aggressor = Side::buy;
        REQUIRE(w.write(tr).has_value());
    }
    REQUIRE(w.close().has_value());
}

backtest::BacktestSpec base_spec(const fs::path& data_dir) {
    auto cfg = Config::parse("[backtest]\nsymbol = ETHUSDT\nfrom = 2024-03-01\nto = 2024-03-09\ninitial_cash = 10000\n"
                             "sample_interval = 1h\n[data]\ndir = " + data_dir.string() +
                             "\n[exchange]\nlatency = zero\n[risk]\nmax_price_deviation = 0\n"
                             "[strategy]\nname = ma_crossover\nlabel = ma\n[strategy.params]\ninterval = 15m\nfast = 4\nslow = 12\nquantity = 1\n");
    REQUIRE(cfg.has_value());
    auto spec = backtest::parse_backtest_spec(*cfg);
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    return *spec;
}

analytics::PerformanceReport fake_report(double ret, double sharpe, double dd, std::size_t trips, double pf,
                                         double bench) {
    analytics::PerformanceReport r;
    r.returns.total_return = ret;
    r.returns.sharpe = sharpe;
    r.returns.max_drawdown = dd;
    r.trades.round_trips = trips;
    r.trades.profit_factor = pf;
    analytics::ReturnMetrics b;
    b.total_return = bench;
    r.benchmark = b;
    r.excess_return = ret - bench;
    return r;
}

}  // namespace

TEST_CASE("metrics and ranking") {
    CHECK(*parse_select_metric("sharpe") == SelectMetric::sharpe);
    CHECK(*parse_select_metric("return") == SelectMetric::total_return);
    CHECK_FALSE(parse_select_metric("luck").has_value());
    CHECK(to_string(SelectMetric::calmar) == "calmar");
    auto a = fake_report(0.10, 1.0, 0.1, 10, 1.5, 0.0);
    auto b = fake_report(0.20, 0.5, 0.2, 10, std::numeric_limits<double>::infinity(), 0.0);
    CHECK(metric_value(a, SelectMetric::total_return) == doctest::Approx(0.10));
    CHECK(metric_value(b, SelectMetric::profit_factor) == doctest::Approx(1e9));
    auto ranked = rank({Ranked{"a", a}, Ranked{"b", b}}, SelectMetric::sharpe);
    CHECK(ranked[0].label == "a");
    ranked = rank({Ranked{"a", a}, Ranked{"b", b}}, SelectMetric::total_return);
    CHECK(ranked[0].label == "b");
    const std::string table = format_comparison(ranked, SelectMetric::total_return);
    CHECK(table.find("sorted by total_return") != std::string::npos);
    CHECK(table.find("b ") != std::string::npos);
}

TEST_CASE("kill criteria") {
    KillCriteria c;
    CHECK(evaluate(fake_report(0.3, 1.2, 0.1, 50, 1.5, 0.1), c).pass);
    auto v = evaluate(fake_report(0.05, 0.2, 0.4, 5, 0.9, 0.1), c);
    CHECK_FALSE(v.pass);
    REQUIRE(v.failures.size() == 5);
    CHECK(v.failures[0].find("round trips") != std::string::npos);
    CHECK(v.failures[1].find("sharpe") != std::string::npos);
    CHECK(v.failures[2].find("drawdown") != std::string::npos);
    CHECK(v.failures[3].find("profit factor") != std::string::npos);
    CHECK(v.failures[4].find("buy-and-hold") != std::string::npos);
    CHECK(format_verdict(v).find("FAIL") == 0);
    CHECK(format_verdict(Verdict{}).find("PASS") == 0);
    c.must_beat_benchmark = false;
    CHECK(evaluate(fake_report(0.05, 1.0, 0.1, 50, 1.5, 0.1), c).pass);
}

TEST_CASE("walk_forward_windows") {
    const Timestamp from = kDay;
    auto w = walk_forward_windows(from, from + Duration::days(10), Duration::days(3), Duration::days(2), Duration::days(2));
    // Windows start at 0, 2, 4 (5 needs train+test = 5 days => 4+5 = 9 <= 10 ok; 6+5 = 11 > 10 no).
    REQUIRE(w.size() == 3);
    CHECK(w[0].train_from == from);
    CHECK(w[0].train_to == from + Duration::days(3));
    CHECK(w[0].test_to == from + Duration::days(5));
    CHECK(w[2].train_from == from + Duration::days(4));
    CHECK(w[2].test_to == from + Duration::days(9));
    CHECK(walk_forward_windows(from, from + Duration::days(4), Duration::days(3), Duration::days(2), Duration::days(1)).empty());
    CHECK(walk_forward_windows(from, from + Duration::days(40), Duration::days(3), Duration{}, Duration::days(1)).empty());
}

TEST_CASE("experiment index append and read") {
    TempDir tmp;
    const fs::path index = tmp.path / "runs" / "index.csv";
    backtest::BacktestResult r;
    r.spec.run_id = "run_a";
    r.spec.from = kDay;
    r.spec.to = kDay + Duration::days(1);
    r.spec.seed = 7;
    backtest::StrategySpec st;
    st.name = "ma_crossover";
    st.label = "ma";
    st.params = *Config::parse("fast = 4\nslow = 12");
    r.spec.strategies.push_back(st);
    r.strategy_labels.emplace_back(StrategyId{1}, "ma");
    auto rep = fake_report(0.12, 1.4, 0.08, 40, 1.6, 0.05);
    REQUIRE(append_to_index(index, r, rep).has_value());
    r.spec.run_id = "run_b";
    rep.returns.total_return = -0.02;
    REQUIRE(append_to_index(index, r, rep).has_value());
    auto rows = read_index(index);
    REQUIRE_MESSAGE(rows.has_value(), rows.error().message);
    REQUIRE(rows->size() == 2);
    CHECK((*rows)[0].run_id == "run_a");
    CHECK((*rows)[0].label == "ma");
    CHECK((*rows)[0].seed == 7);
    CHECK((*rows)[0].total_return == doctest::Approx(0.12));
    CHECK((*rows)[0].sharpe == doctest::Approx(1.4));
    CHECK((*rows)[0].round_trips == 40);
    CHECK((*rows)[0].benchmark_return == doctest::Approx(0.05));
    CHECK((*rows)[0].params == "fast=4;slow=12");
    CHECK((*rows)[1].run_id == "run_b");
    CHECK((*rows)[1].total_return == doctest::Approx(-0.02));
    CHECK_FALSE(read_index(tmp.path / "nope.csv").has_value());
}

TEST_CASE("run_walk_forward: chooses per window in-sample, scores out-of-sample") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    write_store(store);
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto base = base_spec(tmp.path);
    auto sweep = *Config::parse("fast = 3, 6\nslow = 12");
    const auto windows = walk_forward_windows(base.from, base.to, Duration::days(3), Duration::days(2), Duration::days(2));
    REQUIRE(windows.size() == 2);
    auto result = run_walk_forward(base, sweep, windows, SelectMetric::total_return, registry, Logger{}, 2);
    REQUIRE_MESSAGE(result.has_value(), result.error().message);
    REQUIRE(result->windows.size() == 2);
    for (const auto& w : result->windows) {
        CHECK((w.chosen_label == "ma_fast=3_slow=12" || w.chosen_label == "ma_fast=6_slow=12"));
        CHECK(*w.chosen_params.get_int("slow") == 12);
        CHECK(w.out_of_sample.returns.samples > 0);
        CHECK(std::isfinite(w.in_sample_metric));
    }
    CHECK(result->oos_positive_fraction >= 0.0);
    CHECK(result->oos_positive_fraction <= 1.0);
    const double expected = (1.0 + result->windows[0].out_of_sample.returns.total_return) *
                                (1.0 + result->windows[1].out_of_sample.returns.total_return) - 1.0;
    CHECK(result->oos_total_return == doctest::Approx(expected));
    const std::string text = format_walk_forward(*result);
    CHECK(text.find("OOS compounded return") != std::string::npos);

    // Without a sweep the base parameters are used everywhere.
    auto plain = run_walk_forward(base, Config{}, windows, SelectMetric::sharpe, registry, Logger{}, 1);
    REQUIRE(plain.has_value());
    CHECK(plain->windows[0].chosen_label == "ma");
    // Unknown strategy fails cleanly.
    base.strategies[0].name = "nope";
    CHECK_FALSE(run_walk_forward(base, Config{}, windows, SelectMetric::sharpe, registry, Logger{}, 1).has_value());
}
