#pragma once

// Drift detection: is a running strategy still the strategy that research
// approved?
//
// The reference is the performance report of the run that passed the
// go/no-go gate (a backtest or a paper run). The observed run is whatever
// is running now (paper, shadow, testnet or live). Each check compares one
// statistic and grades it ok / warn / alarm against thresholds; the report
// carries the worst grade. A drift alarm is a signal to stop and
// re-validate, not a trading decision by itself.

#include "tradebot/analytics/analytics.hpp"
#include "tradebot/research/research.hpp"
#include "tradebot/research/validation.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tradebot::research {

enum class DriftStatus { ok, warn, alarm };
[[nodiscard]] std::string_view to_string(DriftStatus s) noexcept;

struct DriftThresholds {
    // Return: z-score of the observed total return against the reference's
    // per-period mean and volatility over the same number of periods.
    double return_z_warn = 2.0;
    double return_z_alarm = 3.0;
    // Drawdown: observed max drawdown as a multiple of the reference's. A
    // drawdown a quarter deeper than research saw deserves a look; twice
    // as deep means the strategy is not behaving as tested.
    double drawdown_ratio_warn = 1.25;
    double drawdown_ratio_alarm = 2.0;
    // Costs: observed fee drag (fees / turnover) as a multiple of the reference's.
    double fee_drag_ratio_warn = 1.5;
    double fee_drag_ratio_alarm = 2.5;
    // Activity: round trips per day as a multiple of the reference's (either direction).
    double trade_rate_ratio_warn = 2.0;
    double trade_rate_ratio_alarm = 4.0;
    // Win rate: z-score against the reference's rate with a binomial standard error.
    double win_rate_z_warn = 2.0;
    double win_rate_z_alarm = 3.0;
    std::size_t win_rate_min_trips = 10;
    // Operations (from the observed run's summary).
    double rejection_rate_warn = 0.05;
    double rejection_rate_alarm = 0.20;
    std::uint64_t kill_switch_trips_alarm = 1;
    // Below this many observed samples the statistical checks report ok
    // with an "insufficient data" note instead of a grade.
    std::size_t min_samples = 24;
};

struct DriftCheck {
    std::string name;
    double expected = 0.0;
    double observed = 0.0;
    double score = 0.0;  // z or ratio, whatever the check grades on
    DriftStatus status = DriftStatus::ok;
    std::string detail;
};

struct DriftReport {
    std::string reference_id;
    std::string observed_id;
    Duration observed_span;
    std::size_t observed_samples = 0;
    std::vector<DriftCheck> checks;
    DriftStatus status = DriftStatus::ok;
};

// Operational counters of the observed run (summary.json).
struct ObservedOps {
    std::uint64_t orders = 0;
    std::uint64_t rejected = 0;
    std::uint64_t kill_switch_trips = 0;
};

[[nodiscard]] DriftReport detect_drift(const analytics::PerformanceReport& reference,
                                       const analytics::PerformanceReport& observed,
                                       const std::optional<ObservedOps>& ops = std::nullopt,
                                       const DriftThresholds& thresholds = DriftThresholds{});

// Reads both run directories (equity.csv, fills.csv, summary.json).
[[nodiscard]] Result<DriftReport> detect_drift_dirs(const std::filesystem::path& reference_dir,
                                                    const std::filesystem::path& observed_dir,
                                                    const DriftThresholds& thresholds = DriftThresholds{});

[[nodiscard]] std::string format_drift(const DriftReport& r);
[[nodiscard]] std::string drift_to_json(const DriftReport& r);
// Writes drift.txt and drift.json into the observed run directory.
[[nodiscard]] Result<void> write_drift(const DriftReport& r, const std::filesystem::path& observed_dir);

// --- re-validation ---------------------------------------------------------------

enum class Decision { keep, watch, retire };
[[nodiscard]] std::string_view to_string(Decision d) noexcept;

struct Revalidation {
    DriftReport drift;
    Verdict kill;  // kill criteria applied to the observed run alone
    std::optional<ConsistencyResult> consistency;  // backtest over the observed window, if a spec was given
    Decision decision = Decision::keep;
    std::vector<std::string> reasons;
};

// drift alarm, a kill-criteria failure or an inconsistent backtest ->
// retire; a drift warning -> watch; otherwise keep. The kill criteria are
// only applied once the observed run has at least `criteria.min_round_trips`
// round trips (before that they cannot be judged).
[[nodiscard]] Revalidation revalidate(const DriftReport& drift, const analytics::PerformanceReport& observed,
                                      const KillCriteria& criteria,
                                      std::optional<ConsistencyResult> consistency = std::nullopt);

[[nodiscard]] std::string format_revalidation(const Revalidation& r);
[[nodiscard]] Result<void> write_revalidation(const Revalidation& r, const std::filesystem::path& observed_dir);

}  // namespace tradebot::research
