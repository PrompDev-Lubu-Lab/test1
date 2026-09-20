#include "tradebot/research/research.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>

namespace tradebot::research {

namespace fs = std::filesystem;

// --- metrics ------------------------------------------------------------------

Result<SelectMetric> parse_select_metric(std::string_view text) {
    if (text == "sharpe") return SelectMetric::sharpe;
    if (text == "total_return" || text == "return") return SelectMetric::total_return;
    if (text == "calmar") return SelectMetric::calmar;
    if (text == "profit_factor") return SelectMetric::profit_factor;
    if (text == "sortino") return SelectMetric::sortino;
    return make_error(ErrorCode::parse_error, "unknown metric '" + std::string(text) + "'");
}

std::string_view to_string(SelectMetric m) noexcept {
    switch (m) {
        case SelectMetric::sharpe: return "sharpe";
        case SelectMetric::total_return: return "total_return";
        case SelectMetric::calmar: return "calmar";
        case SelectMetric::profit_factor: return "profit_factor";
        case SelectMetric::sortino: return "sortino";
    }
    return "?";
}

double metric_value(const analytics::PerformanceReport& r, SelectMetric m) {
    switch (m) {
        case SelectMetric::sharpe: return r.returns.sharpe;
        case SelectMetric::total_return: return r.returns.total_return;
        case SelectMetric::calmar: return r.returns.calmar;
        case SelectMetric::profit_factor:
            return std::isfinite(r.trades.profit_factor) ? r.trades.profit_factor : 1e9;
        case SelectMetric::sortino: return r.returns.sortino;
    }
    return 0.0;
}

// --- index --------------------------------------------------------------------

namespace {

std::string params_text(const Config& params) {
    std::string out;
    for (const auto& k : params.keys()) {
        if (!out.empty()) out += ';';
        out += k + "=" + *params.get_string(k);
    }
    return out;
}

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

constexpr const char* kIndexHeader =
    "recorded_at,run_id,label,from,to,seed,total_return,sharpe,max_drawdown,round_trips,benchmark_return,params";

}  // namespace

Result<void> append_to_index(const fs::path& index_csv, const backtest::BacktestResult& result,
                             const analytics::PerformanceReport& report) {
    std::error_code ec;
    fs::create_directories(index_csv.parent_path(), ec);
    const bool fresh = !fs::exists(index_csv) || fs::file_size(index_csv, ec) == 0;
    std::ofstream out(index_csv, std::ios::app);
    if (!out) {
        return make_error(ErrorCode::io_error, "cannot append to " + index_csv.string());
    }
    if (fresh) {
        out << kIndexHeader << '\n';
    }
    const std::string label = result.strategy_labels.empty() ? "" : result.strategy_labels.front().second;
    const std::string params = result.spec.strategies.empty() ? "" : params_text(result.spec.strategies.front().params);
    out << Timestamp::from_chrono(std::chrono::system_clock::now()).to_iso8601() << ',' << result.spec.run_id << ','
        << label << ',' << result.spec.from.to_iso8601() << ',' << result.spec.to.to_iso8601() << ','
        << result.spec.seed << ',' << report.returns.total_return << ',' << report.returns.sharpe << ','
        << report.returns.max_drawdown << ',' << report.trades.round_trips << ','
        << (report.benchmark ? report.benchmark->total_return : 0.0) << ',' << params << '\n';
    return {};
}

Result<std::vector<IndexRow>> read_index(const fs::path& index_csv) {
    std::ifstream in(index_csv);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open " + index_csv.string());
    }
    std::vector<IndexRow> rows;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (first) {
            first = false;
            continue;
        }
        const auto f = split_csv(line);
        if (f.size() < 12) {
            return make_error(ErrorCode::parse_error, "bad index row: " + line);
        }
        IndexRow r;
        r.recorded_at = f[0];
        r.run_id = f[1];
        r.label = f[2];
        r.from = f[3];
        r.to = f[4];
        r.seed = std::stoull(f[5]);
        r.total_return = std::stod(f[6]);
        r.sharpe = std::stod(f[7]);
        r.max_drawdown = std::stod(f[8]);
        r.round_trips = std::stoul(f[9]);
        r.benchmark_return = std::stod(f[10]);
        r.params = f[11];
        rows.push_back(std::move(r));
    }
    return rows;
}

// --- comparison ---------------------------------------------------------------

std::vector<Ranked> rank(std::vector<Ranked> rows, SelectMetric by) {
    std::stable_sort(rows.begin(), rows.end(), [&](const Ranked& a, const Ranked& b) {
        return metric_value(a.report, by) > metric_value(b.report, by);
    });
    return rows;
}

std::string format_comparison(const std::vector<Ranked>& rows, SelectMetric by) {
    std::string s = std::format("{:<48} {:>9} {:>8} {:>8} {:>7} {:>6} {:>8} {:>9}\n", "label", "return%", "sharpe",
                                "max_dd%", "trips", "win%", "pf", "bench%");
    s += std::string("sorted by ") + std::string(to_string(by)) + "\n";
    for (const auto& r : rows) {
        const auto& m = r.report;
        s += std::format("{:<48} {:>9.2f} {:>8.2f} {:>8.2f} {:>7} {:>6.1f} {:>8.2f} {:>9.2f}\n", r.label.substr(0, 48),
                         m.returns.total_return * 100.0, m.returns.sharpe, m.returns.max_drawdown * 100.0,
                         m.trades.round_trips, m.trades.win_rate * 100.0,
                         std::isfinite(m.trades.profit_factor) ? m.trades.profit_factor : 999.0,
                         m.benchmark ? m.benchmark->total_return * 100.0 : 0.0);
    }
    return s;
}

// --- walk-forward ---------------------------------------------------------------

std::vector<WalkForwardWindow> walk_forward_windows(Timestamp from, Timestamp to, Duration train,
                                                    Duration test, Duration step) {
    std::vector<WalkForwardWindow> out;
    if (train.count_nanos() <= 0 || test.count_nanos() <= 0 || step.count_nanos() <= 0) {
        return out;
    }
    for (Timestamp t = from; t + train + test <= to; t += step) {
        out.push_back(WalkForwardWindow{t, t + train, t + train + test});
    }
    return out;
}

Result<WalkForwardResult> run_walk_forward(const backtest::BacktestSpec& base, const Config& sweep,
                                           const std::vector<WalkForwardWindow>& windows,
                                           SelectMetric metric, const strategy::StrategyRegistry& registry,
                                           Logger log, unsigned threads) {
    WalkForwardResult result;
    std::vector<backtest::BacktestSpec> candidates;
    if (sweep.keys().empty()) {
        candidates.push_back(base);
    } else {
        auto expanded = backtest::expand_sweep(base, sweep);
        if (!expanded) {
            return tl::make_unexpected(expanded.error());
        }
        candidates = std::move(*expanded);
    }
    double compounded = 1.0;
    double sharpe_sum = 0.0;
    std::size_t positive = 0;
    for (const auto& w : windows) {
        // In-sample: every candidate on the train range.
        std::vector<backtest::BacktestSpec> train_specs = candidates;
        for (auto& s : train_specs) {
            s.from = w.train_from;
            s.to = w.train_to;
            s.run_id.clear();
        }
        auto train_results = backtest::run_batch(train_specs, registry, log, threads);
        std::size_t best = train_results.size();
        double best_value = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < train_results.size(); ++i) {
            if (!train_results[i]) {
                log.warn("walk-forward: candidate {} failed in-sample: {}", i, train_results[i].error().message);
                continue;
            }
            const double v = metric_value(analytics::analyze(*train_results[i]), metric);
            if (v > best_value) {
                best_value = v;
                best = i;
            }
        }
        if (best == train_results.size()) {
            return make_error(ErrorCode::invalid_state, "walk-forward: no candidate succeeded in-sample");
        }
        // Out-of-sample: the chosen candidate on the test range.
        backtest::BacktestSpec test_spec = candidates[best];
        test_spec.from = w.train_to;
        test_spec.to = w.test_to;
        test_spec.run_id.clear();
        auto oos = backtest::run_backtest(test_spec, registry, log);
        if (!oos) {
            return tl::make_unexpected(oos.error());
        }
        WindowResult wr;
        wr.window = w;
        wr.chosen_label = candidates[best].strategies.front().label;
        wr.chosen_params = candidates[best].strategies.front().params;
        wr.in_sample_metric = best_value;
        wr.out_of_sample = analytics::analyze(*oos);
        compounded *= 1.0 + wr.out_of_sample.returns.total_return;
        sharpe_sum += wr.out_of_sample.returns.sharpe;
        positive += wr.out_of_sample.returns.total_return > 0.0 ? 1 : 0;
        result.oos_worst_drawdown = std::max(result.oos_worst_drawdown, wr.out_of_sample.returns.max_drawdown);
        result.oos_round_trips += wr.out_of_sample.trades.round_trips;
        log.info("walk-forward window {} -> {}: chose {} (IS {} = {:.3f}), OOS return {:+.2f}%",
                 w.train_to.to_iso8601().substr(0, 10), w.test_to.to_iso8601().substr(0, 10), wr.chosen_label,
                 to_string(metric), best_value, wr.out_of_sample.returns.total_return * 100.0);
        result.windows.push_back(std::move(wr));
    }
    if (!result.windows.empty()) {
        result.oos_total_return = compounded - 1.0;
        result.oos_mean_sharpe = sharpe_sum / static_cast<double>(result.windows.size());
        result.oos_positive_fraction = static_cast<double>(positive) / static_cast<double>(result.windows.size());
    }
    return result;
}

std::string format_walk_forward(const WalkForwardResult& r) {
    std::string s = std::format("{:<12} {:<12} {:<40} {:>9} {:>9} {:>8} {:>7}\n", "test_from", "test_to", "chosen",
                                "IS", "OOS ret%", "OOS shp", "trips");
    for (const auto& w : r.windows) {
        s += std::format("{:<12} {:<12} {:<40} {:>9.3f} {:>9.2f} {:>8.2f} {:>7}\n",
                         w.window.train_to.to_iso8601().substr(0, 10), w.window.test_to.to_iso8601().substr(0, 10),
                         w.chosen_label.substr(0, 40), w.in_sample_metric,
                         w.out_of_sample.returns.total_return * 100.0, w.out_of_sample.returns.sharpe,
                         w.out_of_sample.trades.round_trips);
    }
    s += std::format("\nOOS compounded return {:+.2f}%  mean sharpe {:.2f}  positive windows {:.0f}%  worst dd {:.2f}%  round trips {}\n",
                     r.oos_total_return * 100.0, r.oos_mean_sharpe, r.oos_positive_fraction * 100.0,
                     r.oos_worst_drawdown * 100.0, r.oos_round_trips);
    return s;
}

// --- kill criteria ----------------------------------------------------------------

Verdict evaluate(const analytics::PerformanceReport& r, const KillCriteria& c) {
    Verdict v;
    auto fail = [&](std::string why) {
        v.pass = false;
        v.failures.push_back(std::move(why));
    };
    if (r.trades.round_trips < c.min_round_trips) {
        fail(std::format("only {} round trips (need {})", r.trades.round_trips, c.min_round_trips));
    }
    if (r.returns.sharpe < c.min_sharpe) {
        fail(std::format("sharpe {:.2f} below {:.2f}", r.returns.sharpe, c.min_sharpe));
    }
    if (r.returns.max_drawdown > c.max_drawdown) {
        fail(std::format("max drawdown {:.1f}% above {:.1f}%", r.returns.max_drawdown * 100.0, c.max_drawdown * 100.0));
    }
    if (r.trades.profit_factor < c.min_profit_factor) {
        fail(std::format("profit factor {:.2f} below {:.2f}", r.trades.profit_factor, c.min_profit_factor));
    }
    if (c.must_beat_benchmark && r.benchmark && r.excess_return <= 0.0) {
        fail(std::format("does not beat buy-and-hold ({:+.2f}% excess)", r.excess_return * 100.0));
    }
    return v;
}

std::string format_verdict(const Verdict& v) {
    if (v.pass) {
        return "PASS: all kill criteria cleared\n";
    }
    std::string s = "FAIL:\n";
    for (const auto& f : v.failures) {
        s += "  - " + f + "\n";
    }
    return s;
}

}  // namespace tradebot::research
