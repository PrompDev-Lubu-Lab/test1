#include "tradebot/analytics/analytics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>

namespace tradebot::analytics {

namespace fs = std::filesystem;
using backtest::FillRecord;
using portfolio::EquitySample;

namespace {

constexpr double kSecondsPerYear = 365.25 * 86400.0;

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

double stddev(const std::vector<double>& v, double m) {
    if (v.size() < 2) return 0.0;
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}

}  // namespace

ReturnMetrics compute_return_metrics(std::span<const EquitySample> curve) {
    ReturnMetrics m;
    m.samples = curve.size();
    if (curve.size() < 2) {
        return m;
    }
    m.span = curve.back().time - curve.front().time;
    std::vector<double> gaps;
    std::vector<double> rets;
    std::size_t in_market = 0;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        gaps.push_back(static_cast<double>((curve[i].time - curve[i - 1].time).count_nanos()));
        const double prev = curve[i - 1].equity.to_double();
        const double cur = curve[i].equity.to_double();
        if (prev > 0.0) {
            rets.push_back(cur / prev - 1.0);
        }
    }
    for (const auto& s : curve) {
        if (!s.position.is_zero()) ++in_market;
    }
    m.time_in_market = static_cast<double>(in_market) / static_cast<double>(curve.size());
    m.period = Duration::nanos(static_cast<std::int64_t>(median(gaps)));
    const double period_seconds = m.period.as_seconds();
    m.periods_per_year = period_seconds > 0.0 ? kSecondsPerYear / period_seconds : 0.0;

    const double start = curve.front().equity.to_double();
    const double end = curve.back().equity.to_double();
    m.total_return = start > 0.0 ? end / start - 1.0 : 0.0;
    const double years = m.span.as_seconds() / kSecondsPerYear;
    if (years > 0.0 && start > 0.0 && end > 0.0) {
        m.annualized_return = std::pow(end / start, 1.0 / years) - 1.0;
    }
    if (!rets.empty()) {
        const double mu = mean(rets);
        const double sd = stddev(rets, mu);
        m.annualized_volatility = sd * std::sqrt(m.periods_per_year);
        m.sharpe = sd > 0.0 ? mu / sd * std::sqrt(m.periods_per_year) : 0.0;
        std::vector<double> downside;
        for (double r : rets) downside.push_back(std::min(r, 0.0));
        double dd_sq = 0.0;
        for (double d : downside) dd_sq += d * d;
        const double downside_dev = std::sqrt(dd_sq / static_cast<double>(rets.size()));
        m.sortino = downside_dev > 0.0 ? mu / downside_dev * std::sqrt(m.periods_per_year) : 0.0;
        m.best_period = *std::max_element(rets.begin(), rets.end());
        m.worst_period = *std::min_element(rets.begin(), rets.end());
    }
    // Drawdown and its duration.
    double peak = start;
    Timestamp peak_time = curve.front().time;
    Duration longest;
    for (const auto& s : curve) {
        const double e = s.equity.to_double();
        if (e >= peak) {
            peak = e;
            peak_time = s.time;
        } else {
            if (peak > 0.0) m.max_drawdown = std::max(m.max_drawdown, 1.0 - e / peak);
            longest = std::max(longest, s.time - peak_time);
        }
    }
    m.max_drawdown_duration = longest;
    m.calmar = m.max_drawdown > 0.0 ? m.annualized_return / m.max_drawdown : 0.0;
    return m;
}

std::optional<ReturnMetrics> benchmark_buy_and_hold(std::span<const EquitySample> curve,
                                                    Notional initial_cash, execution::FeeRate taker_fee) {
    // Find the first sample with a mark; hold from there.
    std::size_t first = 0;
    while (first < curve.size() && !curve[first].mark.is_positive()) ++first;
    if (first + 1 >= curve.size()) {
        return std::nullopt;
    }
    const Price entry = curve[first].mark;
    const Notional entry_fee = taker_fee.apply(initial_cash);
    const Quantity qty = quantity_for(initial_cash - entry_fee, entry, RoundingMode::down);
    std::vector<EquitySample> bench;
    bench.reserve(curve.size() - first);
    Price last_mark = entry;
    for (std::size_t i = first; i < curve.size(); ++i) {
        EquitySample s = curve[i];
        if (s.mark.is_positive()) last_mark = s.mark;
        s.position = qty;
        s.mark = last_mark;
        s.cash = initial_cash - entry_fee - notional(entry, qty);
        s.fees = entry_fee;
        s.equity = s.cash + notional(last_mark, qty);
        bench.push_back(s);
    }
    // The curve starts from the initial cash (the entry fee is paid in the
    // first period) and pays the exit fee at the end.
    bench.front().equity = initial_cash;
    bench.back().equity -= taker_fee.apply(notional(last_mark, qty));
    return compute_return_metrics(bench);
}

std::vector<RoundTrip> extract_round_trips(std::span<const FillRecord> fills) {
    struct Lot {
        Timestamp time;
        Price price;
        Quantity quantity;
        Notional fee;  // remaining fee attributable to this lot
    };
    struct State {
        std::deque<Lot> lots;
        std::optional<Side> direction;
    };
    std::map<StrategyId, State> states;
    std::vector<RoundTrip> trips;

    for (const auto& f : fills) {
        State& st = states[f.strategy];
        if (!st.direction || st.lots.empty()) {
            st.direction = f.side;
            st.lots.push_back({f.time, f.price, f.quantity, f.fee});
            continue;
        }
        if (f.side == *st.direction) {
            st.lots.push_back({f.time, f.price, f.quantity, f.fee});
            continue;
        }
        // Closing (partially) against FIFO lots.
        Quantity remaining = f.quantity;
        Notional fee_remaining = f.fee;
        while (remaining.is_positive() && !st.lots.empty()) {
            Lot& lot = st.lots.front();
            const Quantity q = std::min(remaining, lot.quantity);
            // Attribute fees proportionally.
            const Notional lot_fee = lot.quantity.is_zero() ? Notional{} : lot.fee.mul_ratio(q.raw(), lot.quantity.raw());
            const Notional close_fee = f.quantity.is_zero() ? Notional{} : f.fee.mul_ratio(q.raw(), f.quantity.raw());
            RoundTrip t;
            t.strategy = f.strategy;
            t.entry_time = lot.time;
            t.exit_time = f.time;
            t.direction = *st.direction;
            t.quantity = q;
            t.entry_price = lot.price;
            t.exit_price = f.price;
            Notional gross = notional(f.price, q) - notional(lot.price, q);
            if (*st.direction == Side::sell) gross = -gross;
            t.gross_pnl = gross;
            t.fees = lot_fee + close_fee;
            t.net_pnl = gross - t.fees;
            const Notional entry_notional = notional(lot.price, q);
            t.return_fraction = entry_notional.is_positive() ? ratio(t.net_pnl, entry_notional) : 0.0;
            t.holding_time = f.time - lot.time;
            trips.push_back(t);
            lot.quantity -= q;
            lot.fee -= lot_fee;
            remaining -= q;
            fee_remaining -= close_fee;
            if (lot.quantity.is_zero()) st.lots.pop_front();
        }
        if (remaining.is_positive()) {
            // Flipped direction: the leftover opens a new position.
            st.direction = f.side;
            st.lots.push_back({f.time, f.price, remaining, fee_remaining});
        }
    }
    std::sort(trips.begin(), trips.end(), [](const RoundTrip& a, const RoundTrip& b) {
        return a.exit_time < b.exit_time;
    });
    return trips;
}

TradeMetrics compute_trade_metrics(const std::vector<RoundTrip>& trips, std::span<const FillRecord> fills) {
    TradeMetrics m;
    m.round_trips = trips.size();
    m.fills = fills.size();
    Duration holding_total;
    for (const auto& t : trips) {
        m.net_pnl += t.net_pnl;
        holding_total += t.holding_time;
        if (t.net_pnl.is_positive()) {
            ++m.wins;
            m.gross_profit += t.net_pnl;
            m.largest_win = std::max(m.largest_win, t.net_pnl);
        } else {
            ++m.losses;
            m.gross_loss += -t.net_pnl;
            m.largest_loss = std::max(m.largest_loss, -t.net_pnl);
        }
    }
    Quantity bought, sold;
    for (const auto& f : fills) {
        m.total_fees += f.fee;
        m.turnover += notional(f.price, f.quantity);
        if (f.side == Side::buy) bought += f.quantity; else sold += f.quantity;
    }
    m.open_quantity = bought - sold;
    if (!trips.empty()) {
        m.win_rate = static_cast<double>(m.wins) / static_cast<double>(trips.size());
        m.expectancy = m.net_pnl.mul_ratio(1, static_cast<std::int64_t>(trips.size()));
        m.average_holding = holding_total / static_cast<std::int64_t>(trips.size());
    }
    if (m.wins > 0) m.average_win = m.gross_profit.mul_ratio(1, static_cast<std::int64_t>(m.wins));
    if (m.losses > 0) m.average_loss = m.gross_loss.mul_ratio(1, static_cast<std::int64_t>(m.losses));
    m.profit_factor = m.gross_loss.is_positive() ? ratio(m.gross_profit, m.gross_loss)
                                                 : (m.gross_profit.is_positive() ? std::numeric_limits<double>::infinity() : 0.0);
    m.fee_drag = m.turnover.is_positive() ? ratio(m.total_fees, m.turnover) : 0.0;
    return m;
}

PerformanceReport analyze(const backtest::BacktestResult& r) {
    PerformanceReport rep;
    rep.run_id = r.spec.run_id;
    rep.returns = compute_return_metrics(r.equity_curve);
    rep.round_trips = extract_round_trips(r.fills);
    rep.trades = compute_trade_metrics(rep.round_trips, r.fills);
    rep.benchmark = benchmark_buy_and_hold(r.equity_curve, r.spec.initial_cash, r.spec.exchange.fees.taker);
    if (rep.benchmark) {
        rep.excess_return = rep.returns.total_return - rep.benchmark->total_return;
    }
    return rep;
}

// --- reading artifacts --------------------------------------------------------

namespace {

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

Result<std::vector<std::string>> read_lines(const fs::path& p) {
    std::ifstream in(p);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open " + p.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

}  // namespace

Result<PerformanceReport> analyze_run_dir(const fs::path& dir) {
    backtest::BacktestResult result;
    auto equity_lines = read_lines(dir / "equity.csv");
    if (!equity_lines) return tl::make_unexpected(equity_lines.error());
    for (std::size_t i = 1; i < equity_lines->size(); ++i) {
        const auto f = split_csv((*equity_lines)[i]);
        if (f.size() < 8) return make_error(ErrorCode::parse_error, "bad equity.csv row " + std::to_string(i));
        EquitySample s;
        auto t = Timestamp::parse_iso8601(f[0]);
        auto eq = Notional::parse(f[1]);
        auto cash = Notional::parse(f[2]);
        auto rp = Notional::parse(f[3]);
        auto up = Notional::parse(f[4]);
        auto fees = Notional::parse(f[5]);
        auto pos = Quantity::parse(f[6]);
        auto mark = Price::parse(f[7]);
        if (!t || !eq || !cash || !rp || !up || !fees || !pos || !mark) {
            return make_error(ErrorCode::parse_error, "bad equity.csv row " + std::to_string(i));
        }
        s.time = *t; s.equity = *eq; s.cash = *cash; s.realized_pnl = *rp; s.unrealized_pnl = *up;
        s.fees = *fees; s.position = *pos; s.mark = *mark;
        result.equity_curve.push_back(s);
    }
    auto fill_lines = read_lines(dir / "fills.csv");
    if (!fill_lines) return tl::make_unexpected(fill_lines.error());
    for (std::size_t i = 1; i < fill_lines->size(); ++i) {
        const auto f = split_csv((*fill_lines)[i]);
        if (f.size() < 8) return make_error(ErrorCode::parse_error, "bad fills.csv row " + std::to_string(i));
        FillRecord r;
        auto t = Timestamp::parse_iso8601(f[0]);
        auto side = parse_side(f[3]);
        auto price = Price::parse(f[4]);
        auto qty = Quantity::parse(f[5]);
        auto fee = Notional::parse(f[6]);
        if (!t || !side || !price || !qty || !fee) {
            return make_error(ErrorCode::parse_error, "bad fills.csv row " + std::to_string(i));
        }
        r.time = *t;
        r.strategy = StrategyId{static_cast<std::uint32_t>(std::stoul(f[1]))};
        r.client_id = ClientOrderId{std::stoull(f[2])};
        r.side = *side; r.price = *price; r.quantity = *qty; r.fee = *fee;
        r.liquidity = f[7] == "maker" ? Liquidity::maker : Liquidity::taker;
        result.fills.push_back(r);
    }
    // summary.json for run id, initial cash and fee.
    std::ifstream sin(dir / "summary.json");
    if (sin) {
        auto j = nlohmann::json::parse(sin, nullptr, false);
        if (j.is_object()) {
            result.spec.run_id = j.value("run_id", dir.filename().string());
            if (auto ic = Notional::parse(j.value("initial_cash", "0")); ic) {
                result.spec.initial_cash = *ic;
            }
        }
    }
    if (result.spec.run_id.empty()) result.spec.run_id = dir.filename().string();
    if (result.spec.initial_cash.is_zero() && !result.equity_curve.empty()) {
        result.spec.initial_cash = result.equity_curve.front().equity;
    }
    // Fee for the benchmark: parse taker from config.txt if present.
    std::ifstream cin(dir / "config.txt");
    std::string line;
    while (cin && std::getline(cin, line)) {
        if (line.rfind("taker_fee = ", 0) == 0) {
            const std::string v = line.substr(12);
            const std::size_t slash = v.find('/');
            if (slash != std::string::npos) {
                result.spec.exchange.fees.taker = execution::FeeRate{std::stoll(v.substr(0, slash)), std::stoll(v.substr(slash + 1))};
            }
        }
    }
    return analyze(result);
}

// --- reporting ----------------------------------------------------------------

std::string format_report(const PerformanceReport& r) {
    const auto& m = r.returns;
    const auto& t = r.trades;
    std::string s;
    s += std::format("Run: {}\n", r.run_id);
    s += std::format("Samples: {}  span: {:.2f} days  period: {}\n", m.samples,
                     m.span.as_seconds() / 86400.0, m.period.to_string());
    s += "\nReturns\n";
    s += std::format("  total return        {:>10.2f}%\n", m.total_return * 100.0);
    s += std::format("  annualized return   {:>10.2f}%\n", m.annualized_return * 100.0);
    s += std::format("  annualized vol      {:>10.2f}%\n", m.annualized_volatility * 100.0);
    s += std::format("  sharpe              {:>10.2f}\n", m.sharpe);
    s += std::format("  sortino             {:>10.2f}\n", m.sortino);
    s += std::format("  calmar              {:>10.2f}\n", m.calmar);
    s += std::format("  max drawdown        {:>10.2f}%  ({} longest)\n", m.max_drawdown * 100.0,
                     m.max_drawdown_duration.to_string());
    s += std::format("  best/worst period   {:>+9.3f}% / {:+.3f}%\n", m.best_period * 100.0, m.worst_period * 100.0);
    s += std::format("  time in market      {:>10.1f}%\n", m.time_in_market * 100.0);
    if (r.benchmark) {
        s += "\nBenchmark (buy & hold)\n";
        s += std::format("  total return        {:>10.2f}%\n", r.benchmark->total_return * 100.0);
        s += std::format("  max drawdown        {:>10.2f}%\n", r.benchmark->max_drawdown * 100.0);
        s += std::format("  sharpe              {:>10.2f}\n", r.benchmark->sharpe);
        s += std::format("  excess return       {:>+10.2f}%\n", r.excess_return * 100.0);
    }
    s += "\nTrades\n";
    s += std::format("  fills / round trips {:>6} / {}\n", t.fills, t.round_trips);
    s += std::format("  win rate            {:>10.1f}%  ({} wins, {} losses)\n", t.win_rate * 100.0, t.wins, t.losses);
    s += std::format("  profit factor       {:>10.2f}\n", t.profit_factor);
    s += std::format("  net pnl             {:>10}\n", t.net_pnl.to_string());
    s += std::format("  expectancy          {:>10}\n", t.expectancy.to_string());
    s += std::format("  avg win / loss      {:>10} / {}\n", t.average_win.to_string(), t.average_loss.to_string());
    s += std::format("  largest win / loss  {:>10} / {}\n", t.largest_win.to_string(), t.largest_loss.to_string());
    s += std::format("  avg holding         {:>10}\n", t.average_holding.to_string());
    s += std::format("  turnover            {:>10}\n", t.turnover.to_string());
    s += std::format("  fees                {:>10}  ({:.3f}% of turnover)\n", t.total_fees.to_string(), t.fee_drag * 100.0);
    s += std::format("  open at end         {:>10}\n", t.open_quantity.to_string());
    return s;
}

nlohmann::json report_to_json(const PerformanceReport& r) {
    auto rm = [](const ReturnMetrics& m) {
        return nlohmann::json{{"samples", m.samples},
                              {"span_seconds", m.span.as_seconds()},
                              {"period_seconds", m.period.as_seconds()},
                              {"total_return", m.total_return},
                              {"annualized_return", m.annualized_return},
                              {"annualized_volatility", m.annualized_volatility},
                              {"sharpe", m.sharpe},
                              {"sortino", m.sortino},
                              {"calmar", m.calmar},
                              {"max_drawdown", m.max_drawdown},
                              {"max_drawdown_duration_seconds", m.max_drawdown_duration.as_seconds()},
                              {"best_period", m.best_period},
                              {"worst_period", m.worst_period},
                              {"time_in_market", m.time_in_market}};
    };
    const auto& t = r.trades;
    nlohmann::json j;
    j["run_id"] = r.run_id;
    j["returns"] = rm(r.returns);
    if (r.benchmark) j["benchmark"] = rm(*r.benchmark);
    j["excess_return"] = r.excess_return;
    j["trades"] = {{"fills", t.fills},
                   {"round_trips", t.round_trips},
                   {"wins", t.wins},
                   {"losses", t.losses},
                   {"win_rate", t.win_rate},
                   {"profit_factor", std::isfinite(t.profit_factor) ? t.profit_factor : -1.0},
                   {"gross_profit", t.gross_profit.to_string()},
                   {"gross_loss", t.gross_loss.to_string()},
                   {"net_pnl", t.net_pnl.to_string()},
                   {"expectancy", t.expectancy.to_string()},
                   {"average_win", t.average_win.to_string()},
                   {"average_loss", t.average_loss.to_string()},
                   {"largest_win", t.largest_win.to_string()},
                   {"largest_loss", t.largest_loss.to_string()},
                   {"average_holding_seconds", t.average_holding.as_seconds()},
                   {"total_fees", t.total_fees.to_string()},
                   {"turnover", t.turnover.to_string()},
                   {"fee_drag", t.fee_drag},
                   {"open_quantity", t.open_quantity.to_string()}};
    return j;
}

Result<void> write_report(const PerformanceReport& r, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / "report.txt", std::ios::trunc);
        if (!out) return make_error(ErrorCode::io_error, "cannot write report.txt");
        out << format_report(r);
    }
    {
        std::ofstream out(dir / "round_trips.csv", std::ios::trunc);
        if (!out) return make_error(ErrorCode::io_error, "cannot write round_trips.csv");
        out << "strategy,entry_time,exit_time,direction,quantity,entry_price,exit_price,gross_pnl,fees,net_pnl,return,holding_seconds\n";
        for (const auto& t : r.round_trips) {
            out << t.strategy.value() << ',' << t.entry_time.to_iso8601() << ',' << t.exit_time.to_iso8601() << ','
                << (t.direction == Side::buy ? "long" : "short") << ',' << t.quantity.to_string() << ','
                << t.entry_price.to_string() << ',' << t.exit_price.to_string() << ',' << t.gross_pnl.to_string() << ','
                << t.fees.to_string() << ',' << t.net_pnl.to_string() << ',' << t.return_fraction << ','
                << t.holding_time.as_seconds() << '\n';
        }
    }
    {
        std::ofstream out(dir / "metrics.json", std::ios::trunc);
        if (!out) return make_error(ErrorCode::io_error, "cannot write metrics.json");
        const nlohmann::json j = report_to_json(r);
        out << j.dump(2) << '\n';
    }
    return {};
}

}  // namespace tradebot::analytics
