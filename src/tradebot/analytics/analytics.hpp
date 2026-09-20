#pragma once

// Performance analytics: honest, comparable numbers from run artifacts.
//
// Return metrics come from the equity curve (period returns between
// samples, annualized by the observed sampling interval). Trade metrics
// come from round trips reconstructed from fills (FIFO). Every run is also
// compared against buy-and-hold on the same marks, because a strategy that
// does not beat holding after costs has no reason to exist. The same
// functions serve backtest, paper and live results, which share the
// artifact format.

#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/portfolio/portfolio.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tradebot::analytics {

struct ReturnMetrics {
    std::size_t samples = 0;
    Duration span;  // first to last sample
    Duration period;  // median sampling interval
    double periods_per_year = 0.0;
    double total_return = 0.0;  // fraction
    double annualized_return = 0.0;  // CAGR
    double annualized_volatility = 0.0;
    double sharpe = 0.0;  // rf = 0
    double sortino = 0.0;
    double calmar = 0.0;
    double max_drawdown = 0.0;  // fraction of peak
    Duration max_drawdown_duration;  // longest peak-to-recovery (or to end)
    double best_period = 0.0;
    double worst_period = 0.0;
    double time_in_market = 0.0;  // fraction of samples with a position
};

struct RoundTrip {
    StrategyId strategy;
    Timestamp entry_time;
    Timestamp exit_time;
    Side direction;  // buy = long
    Quantity quantity;
    Price entry_price;  // volume-weighted
    Price exit_price;
    Notional gross_pnl;
    Notional fees;
    Notional net_pnl;
    double return_fraction = 0.0;  // net / entry notional
    Duration holding_time;
};

struct TradeMetrics {
    std::size_t round_trips = 0;
    std::size_t wins = 0;
    std::size_t losses = 0;
    double win_rate = 0.0;
    Notional gross_profit;
    Notional gross_loss;  // positive number
    double profit_factor = 0.0;  // gross_profit / gross_loss
    Notional net_pnl;
    Notional expectancy;  // net per round trip
    Notional average_win;
    Notional average_loss;  // positive number
    Notional largest_win;
    Notional largest_loss;  // positive number
    Duration average_holding;
    Notional total_fees;
    Notional turnover;
    double fee_drag = 0.0;  // fees / turnover
    std::size_t fills = 0;
    Quantity open_quantity;  // unmatched at the end
};

struct PerformanceReport {
    std::string run_id;
    ReturnMetrics returns;
    TradeMetrics trades;
    std::optional<ReturnMetrics> benchmark;  // buy-and-hold on the same marks
    double excess_return = 0.0;  // total_return - benchmark total_return
    std::vector<RoundTrip> round_trips;
};

[[nodiscard]] ReturnMetrics compute_return_metrics(std::span<const portfolio::EquitySample> curve);

// Buy-and-hold with the same initial cash, entering at the first sample's
// mark and paying the taker fee once each way.
[[nodiscard]] std::optional<ReturnMetrics> benchmark_buy_and_hold(
    std::span<const portfolio::EquitySample> curve, Notional initial_cash,
    execution::FeeRate taker_fee);

[[nodiscard]] std::vector<RoundTrip> extract_round_trips(std::span<const backtest::FillRecord> fills);

[[nodiscard]] TradeMetrics compute_trade_metrics(const std::vector<RoundTrip>& trips,
                                                 std::span<const backtest::FillRecord> fills);

[[nodiscard]] PerformanceReport analyze(const backtest::BacktestResult& result);

// Reads equity.csv, fills.csv and summary.json from a run directory.
[[nodiscard]] Result<PerformanceReport> analyze_run_dir(const std::filesystem::path& dir);

[[nodiscard]] std::string format_report(const PerformanceReport& report);

// Writes metrics.json, report.txt and round_trips.csv into the directory.
[[nodiscard]] Result<void> write_report(const PerformanceReport& report,
                                        const std::filesystem::path& dir);

}  // namespace tradebot::analytics
