#include "tradebot/research/research_json.hpp"

#include <cmath>
#include <fstream>
#include <system_error>

namespace tradebot::research {

namespace fs = std::filesystem;

namespace {

double finite_or(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

nlohmann::json config_to_json(const Config& c) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& k : c.keys()) {
        if (auto v = c.get_string(k)) j[k] = *v;
    }
    return j;
}

}  // namespace

nlohmann::json to_json(const KillCriteria& c) {
    return {{"min_round_trips", c.min_round_trips},
            {"min_sharpe", c.min_sharpe},
            {"max_drawdown", c.max_drawdown},
            {"min_profit_factor", c.min_profit_factor},
            {"must_beat_benchmark", c.must_beat_benchmark}};
}

nlohmann::json to_json(const Verdict& v) {
    return {{"pass", v.pass}, {"failures", v.failures}};
}

nlohmann::json to_json(const MonteCarloResult& r) {
    return {{"samples", r.samples},
            {"trips", r.trips},
            {"return_p05", r.return_p05},
            {"return_p50", r.return_p50},
            {"return_p95", r.return_p95},
            {"drawdown_p50", r.drawdown_p50},
            {"drawdown_p95", r.drawdown_p95},
            {"probability_negative", r.probability_negative}};
}

nlohmann::json to_json(const GridResult& r) {
    nlohmann::json points = nlohmann::json::array();
    for (const auto& p : r.points) {
        points.push_back({{"label", p.label},
                          {"total_return", p.total_return},
                          {"sharpe", finite_or(p.sharpe, 0.0)},
                          {"max_drawdown", p.max_drawdown},
                          {"round_trips", p.round_trips}});
    }
    return {{"points", std::move(points)},
            {"fraction_positive", r.fraction_positive},
            {"median_sharpe", finite_or(r.median_sharpe, 0.0)},
            {"min_sharpe", finite_or(r.min_sharpe, 0.0)},
            {"max_sharpe", finite_or(r.max_sharpe, 0.0)}};
}

nlohmann::json to_json(const RegimeResult& r) {
    nlohmann::json segments = nlohmann::json::array();
    for (const auto& s : r.segments) {
        segments.push_back({{"from", s.from.to_iso8601()},
                            {"to", s.to.to_iso8601()},
                            {"realized_vol", s.realized_vol},
                            {"high_vol", s.high_vol},
                            {"strategy_return", s.strategy_return},
                            {"market_return", s.market_return}});
    }
    return {{"segments", std::move(segments)},
            {"high_vol_return", r.high_vol_return},
            {"low_vol_return", r.low_vol_return},
            {"high_vol_market", r.high_vol_market},
            {"low_vol_market", r.low_vol_market}};
}

nlohmann::json to_json(const ConsistencyResult& r) {
    return {{"from", r.from.to_iso8601()},
            {"to", r.to.to_iso8601()},
            {"backtest_return", r.backtest_return},
            {"paper_return", r.paper_return},
            {"backtest_trips", r.backtest_trips},
            {"paper_trips", r.paper_trips},
            {"return_gap", r.return_gap},
            {"consistent", r.consistent}};
}

nlohmann::json to_json(const WalkForwardResult& r) {
    nlohmann::json windows = nlohmann::json::array();
    for (const auto& w : r.windows) {
        windows.push_back({{"train_from", w.window.train_from.to_iso8601()},
                           {"train_to", w.window.train_to.to_iso8601()},
                           {"test_to", w.window.test_to.to_iso8601()},
                           {"chosen_label", w.chosen_label},
                           {"chosen_params", config_to_json(w.chosen_params)},
                           {"in_sample_metric", finite_or(w.in_sample_metric, 0.0)},
                           {"out_of_sample", analytics::report_to_json(w.out_of_sample)}});
    }
    return {{"windows", std::move(windows)},
            {"oos_total_return", r.oos_total_return},
            {"oos_mean_sharpe", finite_or(r.oos_mean_sharpe, 0.0)},
            {"oos_positive_fraction", r.oos_positive_fraction},
            {"oos_worst_drawdown", r.oos_worst_drawdown},
            {"oos_round_trips", r.oos_round_trips}};
}

nlohmann::json to_json(const GoNoGo& g) {
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& c : g.checks) checks.push_back({{"name", c.name}, {"pass", c.pass}, {"detail", c.detail}});
    return {{"go", g.go}, {"checks", std::move(checks)}};
}

nlohmann::json to_json(const std::vector<Ranked>& rows, SelectMetric by) {
    nlohmann::json out = nlohmann::json::array();
    std::size_t rank = 1;
    for (const auto& r : rows) {
        out.push_back({{"rank", rank++},
                       {"label", r.label},
                       {"metric", finite_or(metric_value(r.report, by), 0.0)},
                       {"report", analytics::report_to_json(r.report)}});
    }
    return {{"sorted_by", std::string(to_string(by))}, {"rows", std::move(out)}};
}

nlohmann::json to_json(const ValidationReport& v) {
    nlohmann::json j;
    j["run_id"] = v.run_id;
    j["report"] = analytics::report_to_json(v.report);
    j["kill_criteria"] = to_json(v.criteria);
    j["verdict"] = to_json(v.verdict);
    if (v.monte_carlo) j["monte_carlo"] = to_json(*v.monte_carlo);
    if (v.costs) j["costs"] = to_json(*v.costs);
    if (v.stability) j["stability"] = to_json(*v.stability);
    if (v.regimes) j["regimes"] = to_json(*v.regimes);
    if (v.walk_forward) j["walk_forward"] = to_json(*v.walk_forward);
    if (v.consistency) j["consistency"] = to_json(*v.consistency);
    j["go_no_go"] = to_json(v.go_no_go);
    return j;
}

Result<void> write_json(const fs::path& file, const nlohmann::json& j) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return make_error(ErrorCode::io_error, "cannot write " + tmp.string());
        out << j.dump(2) << '\n';
        if (!out) return make_error(ErrorCode::io_error, "write failed: " + tmp.string());
    }
    fs::rename(tmp, file, ec);
    if (ec) return make_error(ErrorCode::io_error, "rename failed: " + file.string() + ": " + ec.message());
    return {};
}

}  // namespace tradebot::research
