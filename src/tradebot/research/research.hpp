#pragma once

// Research tooling: the discipline around backtests.
//
//   experiment index   every run's headline numbers appended to one CSV so
//                      results are never re-derived from memory
//   walk-forward       rolling train/test windows; parameters chosen on the
//                      train window are scored on the unseen test window,
//                      which is the only score that counts
//   comparison tables  sweeps ranked by a chosen metric
//   kill criteria      pre-agreed thresholds a strategy must clear; a
//                      strategy that fails them is dropped no matter how
//                      good one backtest looks

#include "tradebot/analytics/analytics.hpp"
#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tradebot::research {

enum class SelectMetric { sharpe, total_return, calmar, profit_factor, sortino };

[[nodiscard]] Result<SelectMetric> parse_select_metric(std::string_view text);
[[nodiscard]] std::string_view to_string(SelectMetric m) noexcept;
[[nodiscard]] double metric_value(const analytics::PerformanceReport& report, SelectMetric metric);

// --- experiment index -------------------------------------------------------

struct IndexRow {
    std::string recorded_at;
    std::string run_id;
    std::string label;
    std::string from;
    std::string to;
    std::uint64_t seed = 0;
    double total_return = 0.0;
    double sharpe = 0.0;
    double max_drawdown = 0.0;
    std::size_t round_trips = 0;
    double benchmark_return = 0.0;
    std::string params;  // "k=v;k=v"
};

[[nodiscard]] Result<void> append_to_index(const std::filesystem::path& index_csv,
                                           const backtest::BacktestResult& result,
                                           const analytics::PerformanceReport& report);
[[nodiscard]] Result<std::vector<IndexRow>> read_index(const std::filesystem::path& index_csv);

// --- comparison -------------------------------------------------------------

struct Ranked {
    std::string label;
    analytics::PerformanceReport report;
};

[[nodiscard]] std::vector<Ranked> rank(std::vector<Ranked> rows, SelectMetric by);
[[nodiscard]] std::string format_comparison(const std::vector<Ranked>& rows, SelectMetric by);

// --- walk-forward -------------------------------------------------------------

struct WalkForwardWindow {
    Timestamp train_from;
    Timestamp train_to;  // == test_from
    Timestamp test_to;
};

// Rolling windows: train [t, t+train), test [t+train, t+train+test), t += step.
// Windows whose test period would end after `to` are dropped.
[[nodiscard]] std::vector<WalkForwardWindow> walk_forward_windows(Timestamp from, Timestamp to,
                                                                  Duration train, Duration test,
                                                                  Duration step);

struct WindowResult {
    WalkForwardWindow window;
    std::string chosen_label;  // best in-sample candidate
    Config chosen_params;
    double in_sample_metric = 0.0;
    analytics::PerformanceReport out_of_sample;
};

struct WalkForwardResult {
    std::vector<WindowResult> windows;
    double oos_total_return = 0.0;  // compounded over test windows
    double oos_mean_sharpe = 0.0;
    double oos_positive_fraction = 0.0;  // windows with positive OOS return
    double oos_worst_drawdown = 0.0;
    std::size_t oos_round_trips = 0;
};

// For each window: run every candidate (base spec x sweep grid, or just the
// base) on the train range, pick the best by `metric`, run it on the test
// range. Candidates are evaluated in parallel on `threads` workers.
[[nodiscard]] Result<WalkForwardResult> run_walk_forward(const backtest::BacktestSpec& base,
                                                         const Config& sweep,
                                                         const std::vector<WalkForwardWindow>& windows,
                                                         SelectMetric metric,
                                                         const strategy::StrategyRegistry& registry,
                                                         Logger log, unsigned threads);

[[nodiscard]] std::string format_walk_forward(const WalkForwardResult& r);

// --- kill criteria ------------------------------------------------------------

struct KillCriteria {
    std::size_t min_round_trips = 30;
    double min_sharpe = 0.5;
    double max_drawdown = 0.25;  // fraction
    double min_profit_factor = 1.1;
    bool must_beat_benchmark = true;
};

struct Verdict {
    bool pass = true;
    std::vector<std::string> failures;
};

[[nodiscard]] Verdict evaluate(const analytics::PerformanceReport& report, const KillCriteria& criteria);
[[nodiscard]] std::string format_verdict(const Verdict& v);

}  // namespace tradebot::research
