#pragma once

// Robust validation: separating edge from noise before risking money.
//
//   monte_carlo      bootstrap the round-trip sequence to get confidence
//                    intervals on return and drawdown
//   cost_sensitivity rerun the strategy under harsher fee, slippage and
//                    latency assumptions; an edge that only exists at zero
//                    cost is not an edge
//   parameter_stability
//                    score every point of a parameter grid; a lone good
//                    point surrounded by bad ones is overfit
//   regime_split     performance in high- versus low-volatility periods,
//                    from one run's equity curve and marks
//   backtest_paper_consistency
//                    the same strategy over the paper-trading window in
//                    backtest versus what paper trading actually did
//   go_no_go         the checklist that combines all of the above

#include "tradebot/analytics/analytics.hpp"
#include "tradebot/backtest/backtest.hpp"
#include "tradebot/research/research.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tradebot::research {

struct MonteCarloResult {
    std::size_t samples = 0;
    std::size_t trips = 0;
    double return_p05 = 0.0;
    double return_p50 = 0.0;
    double return_p95 = 0.0;
    double drawdown_p50 = 0.0;
    double drawdown_p95 = 0.0;
    double probability_negative = 0.0;
};

// Resamples round-trip returns with replacement `samples` times, compounds
// each sequence from initial_cash and reports percentiles of total return
// and max drawdown.
[[nodiscard]] MonteCarloResult monte_carlo_round_trips(const std::vector<analytics::RoundTrip>& trips,
                                                       Notional initial_cash, std::size_t samples,
                                                       std::uint64_t seed);

struct GridPoint {
    std::string label;
    double total_return = 0.0;
    double sharpe = 0.0;
    double max_drawdown = 0.0;
    std::size_t round_trips = 0;
};

struct GridResult {
    std::vector<GridPoint> points;
    double fraction_positive = 0.0;  // points with positive return
    double median_sharpe = 0.0;
    double min_sharpe = 0.0;
    double max_sharpe = 0.0;
};

struct CostGrid {
    std::vector<std::int64_t> fee_bps = {0, 5, 10, 20, 40};
    std::vector<std::int64_t> slippage_bps = {0, 5, 20};
    std::vector<Duration> extra_latency = {Duration{}, Duration::millis(100), Duration::millis(500)};
};

[[nodiscard]] Result<GridResult> cost_sensitivity(const backtest::BacktestSpec& base, const CostGrid& grid,
                                                  const strategy::StrategyRegistry& registry, Logger log,
                                                  unsigned threads);

[[nodiscard]] Result<GridResult> parameter_stability(const backtest::BacktestSpec& base, const Config& sweep,
                                                     const strategy::StrategyRegistry& registry, Logger log,
                                                     unsigned threads);

struct RegimeSegment {
    Timestamp from;
    Timestamp to;
    double realized_vol = 0.0;  // annualized, from marks
    bool high_vol = false;
    double strategy_return = 0.0;
    double market_return = 0.0;
};

struct RegimeResult {
    std::vector<RegimeSegment> segments;
    double high_vol_return = 0.0;  // compounded strategy return over high-vol segments
    double low_vol_return = 0.0;
    double high_vol_market = 0.0;
    double low_vol_market = 0.0;
};

// Splits the equity curve of a finished run into segments of `segment`
// length, classifies each by realized volatility of the marks (above/below
// the median), and compounds the strategy's and the market's returns per
// regime.
[[nodiscard]] RegimeResult regime_split(const backtest::BacktestResult& result, Duration segment);

struct ConsistencyResult {
    Timestamp from;
    Timestamp to;
    double backtest_return = 0.0;
    double paper_return = 0.0;
    std::size_t backtest_trips = 0;
    std::size_t paper_trips = 0;
    double return_gap = 0.0;  // paper - backtest
    bool consistent = false;
};

// Reads a paper-trading run directory, backtests the same spec over the
// paper window and compares. `tolerance` is the acceptable absolute gap in
// total return.
[[nodiscard]] Result<ConsistencyResult> backtest_paper_consistency(const backtest::BacktestSpec& base,
                                                                   const std::filesystem::path& paper_dir,
                                                                   const strategy::StrategyRegistry& registry,
                                                                   Logger log, double tolerance = 0.02);

struct GoNoGoCheck {
    std::string name;
    bool pass = false;
    std::string detail;
};

struct GoNoGo {
    bool go = false;
    std::vector<GoNoGoCheck> checks;
};

struct GoNoGoInputs {
    analytics::PerformanceReport report;
    KillCriteria criteria;
    std::optional<MonteCarloResult> monte_carlo;
    std::optional<GridResult> costs;
    std::optional<GridResult> stability;
    std::optional<WalkForwardResult> walk_forward;
    std::optional<ConsistencyResult> consistency;
};

[[nodiscard]] GoNoGo go_no_go(const GoNoGoInputs& in);

[[nodiscard]] std::string format_monte_carlo(const MonteCarloResult& r);
[[nodiscard]] std::string format_grid(const GridResult& r, std::string_view title);
[[nodiscard]] std::string format_regimes(const RegimeResult& r);
[[nodiscard]] std::string format_consistency(const ConsistencyResult& r);
[[nodiscard]] std::string format_go_no_go(const GoNoGo& g);

}  // namespace tradebot::research
