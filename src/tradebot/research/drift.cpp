#include "tradebot/research/drift.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

namespace tradebot::research {

std::string_view to_string(DriftStatus s) noexcept {
    switch (s) {
        case DriftStatus::ok: return "ok";
        case DriftStatus::warn: return "warn";
        case DriftStatus::alarm: return "alarm";
    }
    return "?";
}

std::string_view to_string(Decision d) noexcept {
    switch (d) {
        case Decision::keep: return "keep";
        case Decision::watch: return "watch";
        case Decision::retire: return "retire";
    }
    return "?";
}

namespace {

DriftStatus grade_abs(double score, double warn, double alarm) {
    const double a = std::abs(score);
    if (a >= alarm) return DriftStatus::alarm;
    if (a >= warn) return DriftStatus::warn;
    return DriftStatus::ok;
}

// Ratio checks are two-sided when `two_sided`: 0.25x is as far as 4x.
DriftStatus grade_ratio(double ratio, double warn, double alarm, bool two_sided) {
    if (!std::isfinite(ratio)) return DriftStatus::ok;
    const double r = two_sided && ratio > 0.0 && ratio < 1.0 ? 1.0 / ratio : ratio;
    if (r >= alarm) return DriftStatus::alarm;
    if (r >= warn) return DriftStatus::warn;
    return DriftStatus::ok;
}

double days(Duration d) { return static_cast<double>(d.count_nanos()) / 86'400e9; }

}  // namespace

DriftReport detect_drift(const analytics::PerformanceReport& ref, const analytics::PerformanceReport& obs,
                         const std::optional<ObservedOps>& ops, const DriftThresholds& th) {
    DriftReport r;
    r.reference_id = ref.run_id;
    r.observed_id = obs.run_id;
    r.observed_span = obs.returns.span;
    r.observed_samples = obs.returns.samples;
    const bool enough = obs.returns.samples >= th.min_samples;
    auto add = [&](DriftCheck c) {
        r.status = std::max(r.status, c.status);
        r.checks.push_back(std::move(c));
    };

    // 1. Return against the reference's distribution.
    {
        DriftCheck c{.name = "return"};
        const double ppy = ref.returns.periods_per_year;
        const double n = static_cast<double>(obs.returns.samples);
        if (enough && ppy > 0.0 && ref.returns.annualized_volatility > 0.0 && n > 1.0) {
            const double mu = std::log1p(ref.returns.annualized_return) / ppy;  // per-period log return
            const double sigma = ref.returns.annualized_volatility / std::sqrt(ppy);
            // Periods are the observed run's own; rescale if its sampling differs.
            const double scale = obs.returns.periods_per_year > 0.0 ? ppy / obs.returns.periods_per_year : 1.0;
            const double periods = n * scale;
            c.expected = std::expm1(mu * periods);
            c.observed = obs.returns.total_return;
            c.score = (std::log1p(obs.returns.total_return) - mu * periods) / (sigma * std::sqrt(periods));
            c.status = grade_abs(c.score, th.return_z_warn, th.return_z_alarm);
            c.detail = std::format("z = {:+.2f} over {} periods", c.score, obs.returns.samples);
        } else {
            c.detail = enough ? "reference has no volatility" : "insufficient data";
        }
        add(std::move(c));
    }
    // 2. Drawdown.
    {
        DriftCheck c{.name = "drawdown"};
        c.expected = ref.returns.max_drawdown;
        c.observed = obs.returns.max_drawdown;
        if (ref.returns.max_drawdown > 0.0) {
            c.score = obs.returns.max_drawdown / ref.returns.max_drawdown;
            c.status = grade_ratio(c.score, th.drawdown_ratio_warn, th.drawdown_ratio_alarm, false);
            c.detail = std::format("{:.2f}x the reference's max drawdown", c.score);
        } else {
            c.score = obs.returns.max_drawdown > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
            c.status = obs.returns.max_drawdown > 0.0 ? DriftStatus::warn : DriftStatus::ok;
            c.detail = "reference had no drawdown";
        }
        add(std::move(c));
    }
    // 3. Fee drag.
    {
        DriftCheck c{.name = "fee_drag"};
        c.expected = ref.trades.fee_drag;
        c.observed = obs.trades.fee_drag;
        if (obs.trades.fills == 0) {
            c.detail = "no fills yet";
        } else if (ref.trades.fee_drag > 0.0) {
            c.score = obs.trades.fee_drag / ref.trades.fee_drag;
            c.status = grade_ratio(c.score, th.fee_drag_ratio_warn, th.fee_drag_ratio_alarm, false);
            c.detail = std::format("{:.2f}x the reference's fees per unit turnover", c.score);
        } else {
            c.detail = "reference paid no fees";
        }
        add(std::move(c));
    }
    // 4. Trade rate.
    {
        DriftCheck c{.name = "trade_rate"};
        const double ref_days = days(ref.returns.span);
        const double obs_days = days(obs.returns.span);
        if (enough && ref_days > 0.0 && obs_days > 0.0 && ref.trades.round_trips > 0) {
            c.expected = static_cast<double>(ref.trades.round_trips) / ref_days;
            c.observed = static_cast<double>(obs.trades.round_trips) / obs_days;
            // Expect at least one round trip before judging a quiet run.
            if (c.expected * obs_days < 1.0) {
                c.detail = "observed window too short for the reference's trade rate";
            } else {
                c.score = c.observed / c.expected;
                c.status = grade_ratio(c.score, th.trade_rate_ratio_warn, th.trade_rate_ratio_alarm, true);
                c.detail = std::format("{:.2f} vs {:.2f} round trips per day", c.observed, c.expected);
            }
        } else {
            c.detail = "insufficient data";
        }
        add(std::move(c));
    }
    // 5. Win rate.
    {
        DriftCheck c{.name = "win_rate"};
        c.expected = ref.trades.win_rate;
        c.observed = obs.trades.win_rate;
        const double n = static_cast<double>(obs.trades.round_trips);
        const double p = ref.trades.win_rate;
        if (obs.trades.round_trips >= th.win_rate_min_trips && p > 0.0 && p < 1.0) {
            c.score = (obs.trades.win_rate - p) / std::sqrt(p * (1.0 - p) / n);
            c.status = grade_abs(c.score, th.win_rate_z_warn, th.win_rate_z_alarm);
            c.detail = std::format("z = {:+.2f} over {} round trips", c.score, obs.trades.round_trips);
        } else {
            c.detail = std::format("need {} round trips (have {})", th.win_rate_min_trips, obs.trades.round_trips);
        }
        add(std::move(c));
    }
    // 6. Operations.
    if (ops) {
        DriftCheck c{.name = "rejections"};
        if (ops->orders > 0) {
            c.observed = static_cast<double>(ops->rejected) / static_cast<double>(ops->orders);
            c.score = c.observed;
            c.status = c.observed >= th.rejection_rate_alarm ? DriftStatus::alarm
                       : c.observed >= th.rejection_rate_warn ? DriftStatus::warn
                                                              : DriftStatus::ok;
            c.detail = std::format("{} of {} orders rejected", ops->rejected, ops->orders);
        } else {
            c.detail = "no orders yet";
        }
        add(std::move(c));
        DriftCheck k{.name = "kill_switch"};
        k.observed = static_cast<double>(ops->kill_switch_trips);
        k.score = k.observed;
        k.status = ops->kill_switch_trips >= th.kill_switch_trips_alarm ? DriftStatus::alarm : DriftStatus::ok;
        k.detail = std::format("{} trip(s)", ops->kill_switch_trips);
        add(std::move(k));
    }
    return r;
}

Result<DriftReport> detect_drift_dirs(const std::filesystem::path& reference_dir,
                                      const std::filesystem::path& observed_dir, const DriftThresholds& th) {
    auto ref = analytics::analyze_run_dir(reference_dir);
    if (!ref) return tl::make_unexpected(ref.error());
    auto obs = analytics::analyze_run_dir(observed_dir);
    if (!obs) return tl::make_unexpected(obs.error());
    std::optional<ObservedOps> ops;
    std::ifstream sin(observed_dir / "summary.json");
    if (sin) {
        auto j = nlohmann::json::parse(sin, nullptr, false);
        if (j.is_object()) {
            ops = ObservedOps{.orders = j.value("orders", std::uint64_t{0}),
                              .rejected = j.value("rejected", std::uint64_t{0}),
                              .kill_switch_trips = j.value("kill_switch_trips", std::uint64_t{0})};
        }
    }
    return detect_drift(*ref, *obs, ops, th);
}

std::string format_drift(const DriftReport& r) {
    std::string s = std::format("Drift: {} vs reference {} ({} samples over {})\n", r.observed_id, r.reference_id,
                                r.observed_samples, r.observed_span.to_string());
    for (const auto& c : r.checks) {
        s += std::format("  {:<12} {:<6} expected {:>10.4f}  observed {:>10.4f}  {}\n", c.name, to_string(c.status),
                         c.expected, c.observed, c.detail);
    }
    s += std::format("Status: {}\n", to_string(r.status));
    return s;
}

std::string drift_to_json(const DriftReport& r) {
    nlohmann::json j;
    j["reference"] = r.reference_id;
    j["observed"] = r.observed_id;
    j["observed_span"] = r.observed_span.to_string();
    j["observed_samples"] = r.observed_samples;
    j["status"] = std::string(to_string(r.status));
    j["checks"] = nlohmann::json::array();
    for (const auto& c : r.checks) {
        j["checks"].push_back({{"name", c.name},
                               {"status", std::string(to_string(c.status))},
                               {"expected", c.expected},
                               {"observed", c.observed},
                               {"score", std::isfinite(c.score) ? c.score : 0.0},
                               {"detail", c.detail}});
    }
    return j.dump(2) + "\n";
}

Result<void> write_drift(const DriftReport& r, const std::filesystem::path& dir) {
    std::ofstream t(dir / "drift.txt", std::ios::trunc);
    std::ofstream j(dir / "drift.json", std::ios::trunc);
    if (!t || !j) {
        return make_error(ErrorCode::io_error, "cannot write drift report into " + dir.string());
    }
    t << format_drift(r);
    j << drift_to_json(r);
    return {};
}

// --- re-validation ---------------------------------------------------------------

Revalidation revalidate(const DriftReport& drift, const analytics::PerformanceReport& observed,
                        const KillCriteria& criteria, std::optional<ConsistencyResult> consistency) {
    Revalidation r;
    r.drift = drift;
    r.consistency = std::move(consistency);
    r.decision = Decision::keep;
    auto worsen = [&](Decision d, std::string why) {
        r.decision = std::max(r.decision, d);
        r.reasons.push_back(std::move(why));
    };
    for (const auto& c : drift.checks) {
        if (c.status == DriftStatus::alarm) worsen(Decision::retire, "drift alarm: " + c.name + " (" + c.detail + ")");
        if (c.status == DriftStatus::warn) worsen(Decision::watch, "drift warning: " + c.name + " (" + c.detail + ")");
    }
    if (observed.trades.round_trips >= criteria.min_round_trips) {
        r.kill = evaluate(observed, criteria);
        for (const auto& f : r.kill.failures) worsen(Decision::retire, "kill criteria: " + f);
    } else {
        r.kill.pass = true;
        r.kill.failures.clear();
    }
    if (r.consistency && !r.consistency->consistent) {
        worsen(Decision::retire, std::format("backtest over the observed window differs by {:+.2f}% return",
                                             r.consistency->return_gap * 100.0));
    }
    return r;
}

std::string format_revalidation(const Revalidation& r) {
    std::string s = format_drift(r.drift);
    s += "Kill criteria: " + format_verdict(r.kill);
    if (r.consistency) s += format_consistency(*r.consistency);
    s += std::format("Decision: {}\n", to_string(r.decision));
    for (const auto& why : r.reasons) s += "  - " + why + "\n";
    return s;
}

Result<void> write_revalidation(const Revalidation& r, const std::filesystem::path& dir) {
    if (auto d = write_drift(r.drift, dir); !d) return d;
    std::ofstream t(dir / "revalidation.txt", std::ios::trunc);
    if (!t) {
        return make_error(ErrorCode::io_error, "cannot write revalidation.txt into " + dir.string());
    }
    t << format_revalidation(r);
    return {};
}

}  // namespace tradebot::research
