#include "tradebot/research/validation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <random>

namespace tradebot::research {

namespace {

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(idx));
    const auto hi = static_cast<std::size_t>(std::ceil(idx));
    const double w = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - w) + v[hi] * w;
}

GridResult summarize(std::vector<GridPoint> points) {
    GridResult g;
    g.points = std::move(points);
    if (g.points.empty()) return g;
    std::vector<double> sharpes;
    std::size_t positive = 0;
    for (const auto& p : g.points) {
        sharpes.push_back(p.sharpe);
        positive += p.total_return > 0.0 ? 1 : 0;
    }
    g.fraction_positive = static_cast<double>(positive) / static_cast<double>(g.points.size());
    g.median_sharpe = percentile(sharpes, 0.5);
    g.min_sharpe = *std::min_element(sharpes.begin(), sharpes.end());
    g.max_sharpe = *std::max_element(sharpes.begin(), sharpes.end());
    return g;
}

Result<GridResult> run_grid(std::vector<backtest::BacktestSpec> specs, std::vector<std::string> labels,
                            const strategy::StrategyRegistry& registry, Logger log, unsigned threads) {
    auto results = backtest::run_batch(specs, registry, log, threads);
    std::vector<GridPoint> points;
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (!results[i]) {
            return tl::make_unexpected(results[i].error());
        }
        const auto rep = analytics::analyze(*results[i]);
        points.push_back(GridPoint{labels[i], rep.returns.total_return, rep.returns.sharpe, rep.returns.max_drawdown,
                                   rep.trades.round_trips});
    }
    return summarize(std::move(points));
}

}  // namespace

// --- Monte Carlo --------------------------------------------------------------

MonteCarloResult monte_carlo_round_trips(const std::vector<analytics::RoundTrip>& trips, Notional initial_cash,
                                         std::size_t samples, std::uint64_t seed) {
    MonteCarloResult r;
    r.samples = samples;
    r.trips = trips.size();
    if (trips.empty() || samples == 0 || !initial_cash.is_positive()) {
        return r;
    }
    // Each trip's P&L as a fraction of the equity at that time is unknown;
    // use net P&L relative to initial cash, which is exact for fixed sizing
    // and conservative otherwise.
    std::vector<double> pnl;
    pnl.reserve(trips.size());
    for (const auto& t : trips) pnl.push_back(t.net_pnl.to_double() / initial_cash.to_double());
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, trips.size() - 1);
    std::vector<double> returns, drawdowns;
    returns.reserve(samples);
    drawdowns.reserve(samples);
    std::size_t negative = 0;
    for (std::size_t s = 0; s < samples; ++s) {
        double equity = 1.0, peak = 1.0, dd = 0.0;
        for (std::size_t i = 0; i < trips.size(); ++i) {
            equity += pnl[pick(rng)];
            peak = std::max(peak, equity);
            if (peak > 0.0) dd = std::max(dd, 1.0 - equity / peak);
        }
        returns.push_back(equity - 1.0);
        drawdowns.push_back(dd);
        negative += equity < 1.0 ? 1 : 0;
    }
    r.return_p05 = percentile(returns, 0.05);
    r.return_p50 = percentile(returns, 0.50);
    r.return_p95 = percentile(returns, 0.95);
    r.drawdown_p50 = percentile(drawdowns, 0.50);
    r.drawdown_p95 = percentile(drawdowns, 0.95);
    r.probability_negative = static_cast<double>(negative) / static_cast<double>(samples);
    return r;
}

// --- Sensitivity grids -----------------------------------------------------------

Result<GridResult> cost_sensitivity(const backtest::BacktestSpec& base, const CostGrid& grid,
                                    const strategy::StrategyRegistry& registry, Logger log, unsigned threads) {
    std::vector<backtest::BacktestSpec> specs;
    std::vector<std::string> labels;
    for (std::int64_t fee : grid.fee_bps) {
        for (std::int64_t slip : grid.slippage_bps) {
            for (Duration lat : grid.extra_latency) {
                backtest::BacktestSpec s = base;
                s.run_id.clear();
                s.exchange.fees.maker = execution::FeeRate::bps(fee);
                s.exchange.fees.taker = execution::FeeRate::bps(fee);
                s.exchange.trade_slippage_bps = slip;
                if (!lat.is_zero()) {
                    if (s.latency.kind == backtest::LatencySpec::Kind::zero) {
                        s.latency.kind = backtest::LatencySpec::Kind::constant;
                    }
                    s.latency.market_data += lat;
                    s.latency.order += lat;
                    s.latency.ack += lat;
                }
                specs.push_back(std::move(s));
                labels.push_back(std::format("fee={}bps slip={}bps +lat={}", fee, slip, lat.to_string()));
            }
        }
    }
    return run_grid(std::move(specs), std::move(labels), registry, log, threads);
}

Result<GridResult> parameter_stability(const backtest::BacktestSpec& base, const Config& sweep,
                                       const strategy::StrategyRegistry& registry, Logger log, unsigned threads) {
    auto specs = backtest::expand_sweep(base, sweep);
    if (!specs) {
        return tl::make_unexpected(specs.error());
    }
    std::vector<std::string> labels;
    for (const auto& s : *specs) labels.push_back(s.strategies.front().label);
    return run_grid(std::move(*specs), std::move(labels), registry, log, threads);
}

// --- Regimes ----------------------------------------------------------------------

RegimeResult regime_split(const backtest::BacktestResult& result, Duration segment) {
    RegimeResult out;
    const auto& curve = result.equity_curve;
    if (curve.size() < 2 || segment.count_nanos() <= 0) {
        return out;
    }
    const double periods_per_year = [&] {
        std::vector<double> gaps;
        for (std::size_t i = 1; i < curve.size(); ++i) gaps.push_back((curve[i].time - curve[i - 1].time).as_seconds());
        std::sort(gaps.begin(), gaps.end());
        const double med = gaps[gaps.size() / 2];
        return med > 0 ? 365.25 * 86400.0 / med : 0.0;
    }();
    std::size_t i = 0;
    while (i + 1 < curve.size()) {
        const Timestamp start = curve[i].time;
        const Timestamp end = start + segment;
        std::size_t j = i;
        while (j + 1 < curve.size() && curve[j + 1].time <= end) ++j;
        if (j == i) {
            ++i;
            continue;
        }
        RegimeSegment seg;
        seg.from = start;
        seg.to = curve[j].time;
        // Realized vol from mark log returns.
        std::vector<double> rets;
        for (std::size_t k = i + 1; k <= j; ++k) {
            const double a = curve[k - 1].mark.to_double();
            const double b = curve[k].mark.to_double();
            if (a > 0.0 && b > 0.0) rets.push_back(std::log(b / a));
        }
        if (rets.size() >= 2) {
            double mu = 0.0;
            for (double r : rets) mu += r;
            mu /= static_cast<double>(rets.size());
            double var = 0.0;
            for (double r : rets) var += (r - mu) * (r - mu);
            var /= static_cast<double>(rets.size() - 1);
            seg.realized_vol = std::sqrt(var) * std::sqrt(periods_per_year);
        }
        const double e0 = curve[i].equity.to_double(), e1 = curve[j].equity.to_double();
        seg.strategy_return = e0 > 0.0 ? e1 / e0 - 1.0 : 0.0;
        const double m0 = curve[i].mark.to_double(), m1 = curve[j].mark.to_double();
        seg.market_return = m0 > 0.0 ? m1 / m0 - 1.0 : 0.0;
        out.segments.push_back(seg);
        i = j;
    }
    if (out.segments.empty()) return out;
    std::vector<double> vols;
    for (const auto& s : out.segments) vols.push_back(s.realized_vol);
    const double median_vol = percentile(vols, 0.5);
    double hi = 1.0, lo = 1.0, hi_m = 1.0, lo_m = 1.0;
    for (auto& s : out.segments) {
        s.high_vol = s.realized_vol > median_vol;
        if (s.high_vol) {
            hi *= 1.0 + s.strategy_return;
            hi_m *= 1.0 + s.market_return;
        } else {
            lo *= 1.0 + s.strategy_return;
            lo_m *= 1.0 + s.market_return;
        }
    }
    out.high_vol_return = hi - 1.0;
    out.low_vol_return = lo - 1.0;
    out.high_vol_market = hi_m - 1.0;
    out.low_vol_market = lo_m - 1.0;
    return out;
}

// --- Consistency ------------------------------------------------------------------

Result<ConsistencyResult> backtest_paper_consistency(const backtest::BacktestSpec& base,
                                                     const std::filesystem::path& paper_dir,
                                                     const strategy::StrategyRegistry& registry, Logger log,
                                                     double tolerance) {
    auto paper = analytics::analyze_run_dir(paper_dir);
    if (!paper) {
        return tl::make_unexpected(paper.error());
    }
    // The paper window comes from its equity curve, via the report's span.
    // analyze_run_dir does not expose the curve; re-read the first/last times.
    std::ifstream in(paper_dir / "equity.csv");
    std::string line, first, last;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (first.empty()) first = line;
        last = line;
    }
    auto from = Timestamp::parse_iso8601(first.substr(0, first.find(',')));
    auto to = Timestamp::parse_iso8601(last.substr(0, last.find(',')));
    if (!from || !to || *to <= *from) {
        return make_error(ErrorCode::parse_error, "paper run has no usable equity window");
    }
    backtest::BacktestSpec spec = base;
    spec.from = *from;
    spec.to = *to + Duration::nanos(1);
    spec.run_id.clear();
    auto bt = backtest::run_backtest(spec, registry, log);
    if (!bt) {
        return tl::make_unexpected(bt.error());
    }
    const auto bt_report = analytics::analyze(*bt);
    ConsistencyResult c;
    c.from = *from;
    c.to = *to;
    c.backtest_return = bt_report.returns.total_return;
    c.paper_return = paper->returns.total_return;
    c.backtest_trips = bt_report.trades.round_trips;
    c.paper_trips = paper->trades.round_trips;
    c.return_gap = c.paper_return - c.backtest_return;
    c.consistent = std::fabs(c.return_gap) <= tolerance;
    return c;
}

// --- Go / no-go ------------------------------------------------------------------------

GoNoGo go_no_go(const GoNoGoInputs& in) {
    GoNoGo g;
    auto add = [&](std::string name, bool pass, std::string detail) {
        g.checks.push_back(GoNoGoCheck{std::move(name), pass, std::move(detail)});
    };
    const Verdict v = evaluate(in.report, in.criteria);
    add("kill criteria", v.pass, v.pass ? "cleared" : v.failures.front());
    if (in.monte_carlo) {
        const auto& mc = *in.monte_carlo;
        add("monte carlo p05 return > 0", mc.return_p05 > 0.0,
            std::format("p05 {:+.2f}%  p50 {:+.2f}%  P(loss) {:.0f}%", mc.return_p05 * 100.0, mc.return_p50 * 100.0,
                        mc.probability_negative * 100.0));
    }
    if (in.costs) {
        add("survives higher costs", in.costs->fraction_positive >= 0.75,
            std::format("{:.0f}% of cost scenarios positive, min sharpe {:.2f}", in.costs->fraction_positive * 100.0,
                        in.costs->min_sharpe));
    }
    if (in.stability) {
        add("parameter neighbourhood", in.stability->fraction_positive >= 0.6 && in.stability->median_sharpe > 0.0,
            std::format("{:.0f}% of grid positive, median sharpe {:.2f}", in.stability->fraction_positive * 100.0,
                        in.stability->median_sharpe));
    }
    if (in.walk_forward) {
        const auto& wf = *in.walk_forward;
        add("walk-forward out-of-sample", wf.oos_total_return > 0.0 && wf.oos_positive_fraction >= 0.5,
            std::format("OOS {:+.2f}%, {:.0f}% windows positive", wf.oos_total_return * 100.0,
                        wf.oos_positive_fraction * 100.0));
    }
    if (in.consistency) {
        add("backtest vs paper", in.consistency->consistent,
            std::format("paper {:+.2f}% vs backtest {:+.2f}%", in.consistency->paper_return * 100.0,
                        in.consistency->backtest_return * 100.0));
    }
    g.go = std::all_of(g.checks.begin(), g.checks.end(), [](const GoNoGoCheck& c) { return c.pass; });
    return g;
}

// --- formatting ----------------------------------------------------------------------

std::string format_monte_carlo(const MonteCarloResult& r) {
    return std::format("Monte Carlo ({} resamples of {} round trips)\n"
                       "  return  p05 {:+.2f}%  p50 {:+.2f}%  p95 {:+.2f}%\n"
                       "  max dd  p50 {:.2f}%  p95 {:.2f}%\n"
                       "  P(negative) {:.1f}%\n",
                       r.samples, r.trips, r.return_p05 * 100.0, r.return_p50 * 100.0, r.return_p95 * 100.0,
                       r.drawdown_p50 * 100.0, r.drawdown_p95 * 100.0, r.probability_negative * 100.0);
}

std::string format_grid(const GridResult& r, std::string_view title) {
    std::string s = std::format("{}\n{:<44} {:>9} {:>8} {:>8} {:>6}\n", title, "point", "return%", "sharpe", "max_dd%", "trips");
    for (const auto& p : r.points) {
        s += std::format("{:<44} {:>9.2f} {:>8.2f} {:>8.2f} {:>6}\n", p.label.substr(0, 44), p.total_return * 100.0, p.sharpe,
                         p.max_drawdown * 100.0, p.round_trips);
    }
    s += std::format("  positive {:.0f}%  sharpe min {:.2f} median {:.2f} max {:.2f}\n", r.fraction_positive * 100.0,
                     r.min_sharpe, r.median_sharpe, r.max_sharpe);
    return s;
}

std::string format_regimes(const RegimeResult& r) {
    std::string s = std::format("Regimes ({} segments)\n{:<12} {:<12} {:>8} {:<8} {:>10} {:>10}\n", r.segments.size(), "from", "to",
                                "vol%", "regime", "strat%", "market%");
    for (const auto& seg : r.segments) {
        s += std::format("{:<12} {:<12} {:>8.1f} {:<8} {:>+10.2f} {:>+10.2f}\n", seg.from.to_iso8601().substr(0, 10),
                         seg.to.to_iso8601().substr(0, 10), seg.realized_vol * 100.0, seg.high_vol ? "high" : "low",
                         seg.strategy_return * 100.0, seg.market_return * 100.0);
    }
    s += std::format("  high-vol: strategy {:+.2f}% market {:+.2f}%   low-vol: strategy {:+.2f}% market {:+.2f}%\n",
                     r.high_vol_return * 100.0, r.high_vol_market * 100.0, r.low_vol_return * 100.0, r.low_vol_market * 100.0);
    return s;
}

std::string format_consistency(const ConsistencyResult& r) {
    return std::format("Backtest vs paper over {} .. {}\n  backtest {:+.2f}% ({} trips)  paper {:+.2f}% ({} trips)  gap {:+.2f}%  {}\n",
                       r.from.to_iso8601().substr(0, 16), r.to.to_iso8601().substr(0, 16), r.backtest_return * 100.0,
                       r.backtest_trips, r.paper_return * 100.0, r.paper_trips, r.return_gap * 100.0,
                       r.consistent ? "CONSISTENT" : "INCONSISTENT");
}

std::string format_go_no_go(const GoNoGo& g) {
    std::string s = g.go ? "GO\n" : "NO-GO\n";
    for (const auto& c : g.checks) {
        s += std::format("  [{}] {:<32} {}\n", c.pass ? "pass" : "FAIL", c.name, c.detail);
    }
    return s;
}

}  // namespace tradebot::research
