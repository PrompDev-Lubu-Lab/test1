#include "tradebot/analytics/analytics.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>

using namespace tradebot;
using namespace tradebot::analytics;
using namespace tradebot::backtest;
using namespace tradebot::portfolio;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

const Timestamp kT0 = *Timestamp::parse_iso8601("2024-01-01");

EquitySample sample(int day, double equity, double mark = 100.0, double position = 0.0) {
    EquitySample s;
    s.time = kT0 + Duration::days(day);
    s.equity = Notional::from_double(equity);
    s.cash = s.equity;
    s.mark = Price::from_double(mark);
    s.position = Quantity::from_double(position);
    return s;
}

FillRecord fill(int minute, Side side, double price, double qty, double fee = 0.0, std::uint32_t strategy = 1) {
    FillRecord f;
    f.time = kT0 + Duration::minutes(minute);
    f.strategy = StrategyId{strategy};
    f.client_id = ClientOrderId{static_cast<std::uint64_t>(minute)};
    f.side = side;
    f.price = Price::from_double(price);
    f.quantity = Quantity::from_double(qty);
    f.fee = Notional::from_double(fee);
    f.liquidity = Liquidity::taker;
    return f;
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-an-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

}  // namespace

TEST_CASE("compute_return_metrics: daily curve") {
    // 10000 -> 10100 -> 10000 -> 10200 -> 10100 over 4 days, positions on days 1-2.
    std::vector<EquitySample> curve{sample(0, 10000), sample(1, 10100, 100, 1), sample(2, 10000, 100, 1),
                                    sample(3, 10200), sample(4, 10100)};
    auto m = compute_return_metrics(curve);
    CHECK(m.samples == 5);
    CHECK(m.span == Duration::days(4));
    CHECK(m.period == Duration::days(1));
    CHECK(m.periods_per_year == doctest::Approx(365.25));
    CHECK(m.total_return == doctest::Approx(0.01));
    CHECK(m.annualized_return == doctest::Approx(std::pow(1.01, 365.25 / 4.0) - 1.0));
    // Period returns: +1%, -0.990%, +2%, -0.980%
    const std::vector<double> r{0.01, 10000.0 / 10100.0 - 1.0, 0.02, 10100.0 / 10200.0 - 1.0};
    double mu = 0;
    for (double x : r) mu += x;
    mu /= 4.0;
    double var = 0;
    for (double x : r) var += (x - mu) * (x - mu);
    const double sd = std::sqrt(var / 3.0);
    CHECK(m.annualized_volatility == doctest::Approx(sd * std::sqrt(365.25)));
    CHECK(m.sharpe == doctest::Approx(mu / sd * std::sqrt(365.25)));
    CHECK(m.best_period == doctest::Approx(0.02));
    CHECK(m.worst_period == doctest::Approx(10000.0 / 10100.0 - 1.0));
    // Max drawdown: peak 10100 -> 10000 = 0.99%; then peak 10200 -> 10100 = 0.98%.
    CHECK(m.max_drawdown == doctest::Approx(1.0 - 10000.0 / 10100.0));
    CHECK(m.max_drawdown_duration == Duration::days(1));
    CHECK(m.calmar == doctest::Approx(m.annualized_return / m.max_drawdown));
    CHECK(m.time_in_market == doctest::Approx(0.4));
    CHECK(m.sortino > m.sharpe);

    // Degenerate inputs.
    CHECK(compute_return_metrics({}).samples == 0);
    std::vector<EquitySample> one{sample(0, 1)};
    CHECK(compute_return_metrics(one).total_return == 0.0);
    std::vector<EquitySample> flat{sample(0, 100), sample(1, 100), sample(2, 100)};
    auto f = compute_return_metrics(flat);
    CHECK(f.sharpe == 0.0);
    CHECK(f.max_drawdown == 0.0);
    CHECK(f.calmar == 0.0);
}

TEST_CASE("compute_return_metrics: drawdown duration spans until recovery") {
    std::vector<EquitySample> curve{sample(0, 100), sample(1, 90), sample(2, 95), sample(3, 99), sample(4, 101), sample(5, 100)};
    auto m = compute_return_metrics(curve);
    CHECK(m.max_drawdown == doctest::Approx(0.10));
    CHECK(m.max_drawdown_duration == Duration::days(3));  // day 0 peak, underwater through day 3
}

TEST_CASE("benchmark_buy_and_hold") {
    std::vector<EquitySample> curve{sample(0, 10000, 100), sample(1, 10000, 110), sample(2, 10000, 120)};
    auto b = benchmark_buy_and_hold(curve, "10000"_ntl, execution::FeeRate::bps(10));
    REQUIRE(b.has_value());
    // Entry fee 10; buy 99.9 units at 100; final 99.9*120 = 11988 minus exit fee 11.988 -> 11976.012
    CHECK(b->total_return == doctest::Approx(11976.012 / 10000.0 - 1.0).epsilon(1e-6));
    CHECK(b->samples == 3);
    CHECK(b->time_in_market == doctest::Approx(1.0));
    std::vector<EquitySample> no_marks{sample(0, 1, 0), sample(1, 1, 0)};
    CHECK_FALSE(benchmark_buy_and_hold(no_marks, "1"_ntl, execution::FeeRate::zero()).has_value());
}

TEST_CASE("extract_round_trips: FIFO matching, partial closes, flips, fees, strategies") {
    std::vector<FillRecord> fills{
        fill(0, Side::buy, 100, 1, 0.1),
        fill(10, Side::buy, 110, 1, 0.11),
        fill(20, Side::sell, 120, 1.5, 0.18),  // closes lot1 fully and half of lot2
        fill(30, Side::sell, 90, 1.0, 0.09),  // closes rest of lot2 (0.5) and opens short 0.5 @ 90
        fill(40, Side::buy, 80, 0.5, 0.04),  // covers the short: +5
        fill(5, Side::buy, 50, 2, 0, 2),  // strategy 2, never closed
    };
    auto trips = extract_round_trips(fills);
    REQUIRE(trips.size() == 4);
    // Sorted by exit time.
    CHECK(trips[0].entry_price == "100"_px);
    CHECK(trips[0].exit_price == "120"_px);
    CHECK(trips[0].quantity == "1"_qty);
    CHECK(trips[0].gross_pnl == "20"_ntl);
    CHECK(trips[0].fees == "0.22"_ntl);  // 0.1 + 0.18 * (1/1.5)
    CHECK(trips[0].net_pnl == "19.78"_ntl);
    CHECK(trips[0].holding_time == Duration::minutes(20));
    CHECK(trips[0].direction == Side::buy);
    CHECK(trips[0].return_fraction == doctest::Approx(0.1978));

    CHECK(trips[1].entry_price == "110"_px);
    CHECK(trips[1].quantity == "0.5"_qty);
    CHECK(trips[1].gross_pnl == "5"_ntl);
    CHECK(trips[1].fees == "0.115"_ntl);  // 0.055 + 0.06

    CHECK(trips[2].entry_price == "110"_px);
    CHECK(trips[2].exit_price == "90"_px);
    CHECK(trips[2].gross_pnl == "-10"_ntl);
    CHECK(trips[2].fees == "0.1"_ntl);
    CHECK(trips[2].exit_time == kT0 + Duration::minutes(30));

    CHECK(trips[3].direction == Side::sell);  // the flipped short
    CHECK(trips[3].entry_price == "90"_px);
    CHECK(trips[3].exit_price == "80"_px);
    CHECK(trips[3].gross_pnl == "5"_ntl);
    CHECK(trips[3].fees == "0.085"_ntl);  // 0.045 (half of 0.09) + 0.04

    auto tm = compute_trade_metrics(trips, fills);
    CHECK(tm.round_trips == 4);
    CHECK(tm.fills == 6);
    CHECK(tm.wins == 3);
    CHECK(tm.losses == 1);
    CHECK(tm.win_rate == doctest::Approx(0.75));
    CHECK(tm.gross_profit == "19.78"_ntl + "4.885"_ntl + "4.915"_ntl);
    CHECK(tm.gross_loss == "10.1"_ntl);  // -10 gross, 0.055 remaining entry fee + 0.045 exit fee
    CHECK(tm.net_pnl == tm.gross_profit - tm.gross_loss);
    CHECK(tm.profit_factor == doctest::Approx(ratio(tm.gross_profit, tm.gross_loss)));
    CHECK(tm.largest_win == "19.78"_ntl);
    CHECK(tm.largest_loss == "10.1"_ntl);
    CHECK(tm.average_holding == (Duration::minutes(20) + Duration::minutes(10) + Duration::minutes(20) + Duration::minutes(10)) / 4);
    CHECK(tm.total_fees == "0.52"_ntl);
    CHECK(tm.turnover == "100"_ntl + "110"_ntl + "180"_ntl + "90"_ntl + "40"_ntl + "100"_ntl);
    CHECK(tm.open_quantity == "2"_qty);  // strategy 2's unmatched long
    CHECK(tm.fee_drag == doctest::Approx(0.52 / 620.0));

    TradeMetrics none = compute_trade_metrics({}, {});
    CHECK(none.win_rate == 0.0);
    CHECK(none.profit_factor == 0.0);
}

TEST_CASE("analyze, write_report and analyze_run_dir round trip") {
    TempDir tmp;
    BacktestResult r;
    r.spec.run_id = "test_run";
    r.spec.initial_cash = "10000"_ntl;
    r.spec.exchange.fees.taker = execution::FeeRate::bps(10);
    r.equity_curve = {sample(0, 10000, 100), sample(1, 10050, 101, 1), sample(2, 10020, 99, 1), sample(3, 10100, 103)};
    r.fills = {fill(10, Side::buy, 100, 1, 0.1), fill(3000, Side::sell, 103, 1, 0.103)};
    auto rep = analyze(r);
    CHECK(rep.run_id == "test_run");
    CHECK(rep.returns.total_return == doctest::Approx(0.01));
    REQUIRE(rep.benchmark.has_value());
    CHECK(rep.benchmark->total_return > 0.02);  // 100 -> 103 minus fees
    CHECK(rep.excess_return < 0.0);
    REQUIRE(rep.round_trips.size() == 1);
    CHECK(rep.trades.net_pnl == "2.797"_ntl);
    const std::string text = format_report(rep);
    CHECK(text.find("Run: test_run") != std::string::npos);
    CHECK(text.find("Benchmark") != std::string::npos);
    CHECK(text.find("profit factor") != std::string::npos);

    // Write backtest artifacts, then analyze the directory.
    REQUIRE(write_artifacts(r, tmp.path).has_value());
    REQUIRE(write_report(rep, tmp.path).has_value());
    CHECK(fs::exists(tmp.path / "metrics.json"));
    CHECK(fs::exists(tmp.path / "report.txt"));
    CHECK(fs::exists(tmp.path / "round_trips.csv"));
    auto again = analyze_run_dir(tmp.path);
    REQUIRE_MESSAGE(again.has_value(), again.error().message);
    CHECK(again->run_id == "test_run");
    CHECK(again->returns.total_return == doctest::Approx(rep.returns.total_return));
    CHECK(again->returns.sharpe == doctest::Approx(rep.returns.sharpe));
    CHECK(again->trades.net_pnl == rep.trades.net_pnl);
    REQUIRE(again->benchmark.has_value());
    CHECK(again->benchmark->total_return == doctest::Approx(rep.benchmark->total_return));
    CHECK_FALSE(analyze_run_dir(tmp.path / "missing").has_value());
}
