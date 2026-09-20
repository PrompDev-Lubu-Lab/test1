#include "tradebot/research/validation.hpp"

#include "tradebot/strategies/baselines.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
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
        path = fs::temp_directory_path() / ("tradebot-val-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const InstrumentId kEth{1};
const Timestamp kDay = *Timestamp::parse_iso8601("2024-03-01");

void write_store(const StorePath& store, int days) {
    EventStoreWriter w(store, kEth, DataSource::bulk);
    std::uint64_t id = 1;
    for (int i = 0; i < days * 24 * 60; ++i) {
        // A 12h sine plus a slow drift; amplitude doubles in the second half (regime change).
        const double amp = i < days * 12 * 60 ? 50.0 : 100.0;
        const double mid = 3000.0 + amp * std::sin(2.0 * 3.14159265358979 * i / 720.0) + 0.05 * i / 60.0;
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

backtest::BacktestSpec base_spec(const fs::path& data_dir, const char* to = "2024-03-05") {
    auto cfg = Config::parse("[backtest]\nsymbol = ETHUSDT\nfrom = 2024-03-01\nto = " + std::string(to) +
                             "\ninitial_cash = 10000\nsample_interval = 1h\n[data]\ndir = " + data_dir.string() +
                             "\n[exchange]\nlatency = zero\n[risk]\nmax_price_deviation = 0\n"
                             "[strategy]\nname = ma_crossover\nlabel = ma\n[strategy.params]\ninterval = 15m\nfast = 4\nslow = 12\nquantity = 1\n");
    REQUIRE(cfg.has_value());
    auto spec = backtest::parse_backtest_spec(*cfg);
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    return *spec;
}

analytics::RoundTrip trip(double net) {
    analytics::RoundTrip t;
    t.net_pnl = Notional::from_double(net);
    t.quantity = "1"_qty;
    return t;
}

}  // namespace

TEST_CASE("monte_carlo_round_trips: percentiles and probability of loss") {
    std::vector<analytics::RoundTrip> wins;
    for (int i = 0; i < 20; ++i) wins.push_back(trip(100.0));
    auto mc = monte_carlo_round_trips(wins, "10000"_ntl, 500, 1);
    CHECK(mc.samples == 500);
    CHECK(mc.trips == 20);
    CHECK(mc.return_p05 == doctest::Approx(0.20));
    CHECK(mc.return_p95 == doctest::Approx(0.20));
    CHECK(mc.drawdown_p95 == doctest::Approx(0.0));
    CHECK(mc.probability_negative == 0.0);

    std::vector<analytics::RoundTrip> mixed;
    for (int i = 0; i < 30; ++i) mixed.push_back(trip(i % 2 ? 150.0 : -100.0));
    auto m2 = monte_carlo_round_trips(mixed, "10000"_ntl, 2000, 7);
    CHECK(m2.return_p05 < m2.return_p50);
    CHECK(m2.return_p50 < m2.return_p95);
    CHECK(m2.return_p50 == doctest::Approx(0.075).epsilon(0.5));  // expectation 25 * 30 / 10000
    CHECK(m2.drawdown_p95 > 0.0);
    CHECK(m2.probability_negative > 0.0);
    CHECK(m2.probability_negative < 0.5);
    auto same = monte_carlo_round_trips(mixed, "10000"_ntl, 2000, 7);
    CHECK(same.return_p50 == m2.return_p50);  // seeded
    CHECK(monte_carlo_round_trips({}, "1"_ntl, 10, 1).trips == 0);
}

TEST_CASE("go_no_go and formatting") {
    analytics::PerformanceReport rep;
    rep.returns.total_return = 0.2;
    rep.returns.sharpe = 1.5;
    rep.returns.max_drawdown = 0.1;
    rep.trades.round_trips = 50;
    rep.trades.profit_factor = 1.8;
    analytics::ReturnMetrics bench;
    bench.total_return = 0.05;
    rep.benchmark = bench;
    rep.excess_return = 0.15;
    GoNoGoInputs in;
    in.report = rep;
    in.monte_carlo = MonteCarloResult{.samples = 100, .trips = 50, .return_p05 = 0.02, .return_p50 = 0.2, .return_p95 = 0.4};
    in.costs = GridResult{.fraction_positive = 0.9, .median_sharpe = 1.0, .min_sharpe = 0.3};
    in.stability = GridResult{.fraction_positive = 0.7, .median_sharpe = 0.8};
    WalkForwardResult wf;
    wf.oos_total_return = 0.05;
    wf.oos_positive_fraction = 0.6;
    in.walk_forward = wf;
    in.consistency = ConsistencyResult{.backtest_return = 0.03, .paper_return = 0.02, .return_gap = -0.01, .consistent = true};
    auto g = go_no_go(in);
    CHECK(g.go);
    CHECK(g.checks.size() == 6);
    CHECK(format_go_no_go(g).find("GO\n") == 0);

    in.monte_carlo->return_p05 = -0.01;
    in.stability->fraction_positive = 0.3;
    g = go_no_go(in);
    CHECK_FALSE(g.go);
    int fails = 0;
    for (const auto& c : g.checks) fails += !c.pass;
    CHECK(fails == 2);
    CHECK(format_go_no_go(g).find("NO-GO") == 0);
    CHECK(format_monte_carlo(*in.monte_carlo).find("Monte Carlo") == 0);
    CHECK(format_grid(*in.costs, "Costs").find("Costs") == 0);
    CHECK(format_consistency(*in.consistency).find("CONSISTENT") != std::string::npos);
}

TEST_CASE("regime_split: segments by realized volatility from a run's marks") {
    backtest::BacktestResult r;
    // 4 days hourly: first two days calm marks, last two volatile.
    std::mt19937_64 rng(3);
    std::normal_distribution<double> calm(0.0, 0.001), wild(0.0, 0.01);
    double mark = 100.0, equity = 10000.0;
    for (int h = 0; h <= 96; ++h) {
        portfolio::EquitySample s;
        s.time = kDay + Duration::hours(h);
        s.mark = Price::from_double(mark);
        s.equity = Notional::from_double(equity);
        r.equity_curve.push_back(s);
        mark *= 1.0 + (h < 48 ? calm(rng) : wild(rng));
        equity *= 1.0 + (h < 48 ? 0.0005 : -0.0005);  // gains in calm, losses in wild
    }
    auto reg = regime_split(r, Duration::days(1));
    REQUIRE(reg.segments.size() == 4);
    CHECK_FALSE(reg.segments[0].high_vol);
    CHECK_FALSE(reg.segments[1].high_vol);
    CHECK(reg.segments[2].high_vol);
    CHECK(reg.segments[3].high_vol);
    CHECK(reg.segments[2].realized_vol > 5 * reg.segments[0].realized_vol);
    CHECK(reg.low_vol_return > 0.0);
    CHECK(reg.high_vol_return < 0.0);
    CHECK(format_regimes(reg).find("Regimes (4 segments)") == 0);
    CHECK(regime_split(backtest::BacktestResult{}, Duration::days(1)).segments.empty());
}

TEST_CASE("cost_sensitivity and parameter_stability on a synthetic store") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    write_store(store, 4);
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto base = base_spec(tmp.path);

    CostGrid grid;
    grid.fee_bps = {0, 50};
    grid.slippage_bps = {0, 30};
    grid.extra_latency = {Duration{}};
    auto costs = cost_sensitivity(base, grid, registry, Logger{}, 2);
    REQUIRE_MESSAGE(costs.has_value(), costs.error().message);
    REQUIRE(costs->points.size() == 4);
    CHECK(costs->points[0].label.find("fee=0bps slip=0bps") == 0);
    // Higher costs never improve the return.
    CHECK(costs->points[3].total_return < costs->points[0].total_return);
    CHECK(costs->min_sharpe <= costs->max_sharpe);
    CHECK(costs->points[0].round_trips == costs->points[3].round_trips);

    auto stab = parameter_stability(base, *Config::parse("fast = 3, 5\nslow = 12"), registry, Logger{}, 2);
    REQUIRE_MESSAGE(stab.has_value(), stab.error().message);
    REQUIRE(stab->points.size() == 2);
    CHECK(stab->points[0].label == "ma_fast=3_slow=12");
    CHECK(stab->fraction_positive >= 0.0);
    CHECK_FALSE(parameter_stability(base, *Config::parse("fast = "), registry, Logger{}, 1).has_value());
}

TEST_CASE("backtest_paper_consistency: same window, same data => consistent") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    write_store(store, 4);
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto base = base_spec(tmp.path);
    // Fake a paper run: a backtest over a sub-window written as artifacts.
    auto paper_spec = base;
    paper_spec.from = kDay + Duration::days(1);
    paper_spec.to = kDay + Duration::days(3);
    auto paper = backtest::run_backtest(paper_spec, registry, Logger{});
    REQUIRE(paper.has_value());
    const fs::path paper_dir = tmp.path / "paper";
    REQUIRE(backtest::write_artifacts(*paper, paper_dir).has_value());
    auto c = backtest_paper_consistency(base, paper_dir, registry, Logger{}, 0.001);
    REQUIRE_MESSAGE(c.has_value(), c.error().message);
    CHECK(c->consistent);
    CHECK(c->from == paper_spec.from);
    CHECK(std::fabs(c->return_gap) < 1e-9);
    CHECK(c->backtest_trips == c->paper_trips);
    CHECK_FALSE(backtest_paper_consistency(base, tmp.path / "nope", registry, Logger{}).has_value());
}
