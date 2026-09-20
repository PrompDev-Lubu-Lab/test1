#include "tradebot/research/drift.hpp"

#include "tradebot/backtest/backtest.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>

using namespace tradebot;
using namespace tradebot::research;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

const Timestamp kStart = *Timestamp::parse_iso8601("2024-03-01");

// An hourly equity curve with the given drift and volatility per period,
// plus round trips every `trip_every` samples with the given win rate and
// fee per fill. Deterministic for a seed.
backtest::BacktestResult synthetic(std::string run_id, int samples, double mu, double sigma, int trip_every,
                                   double win_rate, const char* fee, std::uint64_t seed) {
    backtest::BacktestResult r;
    r.spec.run_id = std::move(run_id);
    r.spec.initial_cash = "10000"_ntl;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> noise(mu, sigma);
    std::bernoulli_distribution win(win_rate);
    double equity = 10000.0;
    std::uint64_t cid = 1;
    for (int i = 0; i < samples; ++i) {
        const Timestamp t = kStart + Duration::hours(i);
        equity *= 1.0 + noise(rng);
        portfolio::EquitySample s;
        s.time = t;
        s.equity = Notional::from_double(equity);
        s.cash = s.equity;
        s.mark = "3000"_px;
        s.position = (i / trip_every) % 2 == 0 ? Quantity{} : "1"_qty;
        r.equity_curve.push_back(s);
        if (trip_every > 0 && i % trip_every == trip_every - 1) {
            const bool w = win(rng);
            backtest::FillRecord buy{t, StrategyId{1}, ClientOrderId{cid++}, Side::buy, "3000"_px, "1"_qty,
                                     *Notional::parse(fee), Liquidity::taker};
            backtest::FillRecord sell{t + Duration::minutes(30), StrategyId{1}, ClientOrderId{cid++}, Side::sell,
                                      w ? "3010"_px : "2995"_px, "1"_qty, *Notional::parse(fee), Liquidity::taker};
            r.fills.push_back(buy);
            r.fills.push_back(sell);
        }
    }
    r.summary.initial_cash = r.spec.initial_cash;
    r.summary.final_equity = r.equity_curve.back().equity;
    r.summary.orders = r.fills.size();
    r.summary.fills = r.fills.size();
    return r;
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-drift-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const DriftCheck& check(const DriftReport& r, std::string_view name) {
    for (const auto& c : r.checks) {
        if (c.name == name) return c;
    }
    static const DriftCheck none;
    return none;
}

}  // namespace

TEST_CASE("detect_drift: a run drawn from the reference's distribution is ok") {
    auto ref = analytics::analyze(synthetic("ref", 24 * 60, 0.0002, 0.004, 12, 0.55, "3", 1));
    auto obs = analytics::analyze(synthetic("obs", 24 * 10, 0.0002, 0.004, 12, 0.55, "3", 2));
    auto d = detect_drift(ref, obs, ObservedOps{.orders = 40, .rejected = 0, .kill_switch_trips = 0});
    CHECK(d.reference_id == "ref");
    CHECK(d.observed_id == "obs");
    CHECK(d.observed_samples == 240);
    CHECK(d.status == DriftStatus::ok);
    CHECK(d.checks.size() == 7);
    CHECK(std::abs(check(d, "return").score) < 2.0);
    CHECK(check(d, "drawdown").status == DriftStatus::ok);
    CHECK(check(d, "fee_drag").score == doctest::Approx(1.0).epsilon(0.05));
    CHECK(check(d, "trade_rate").score == doctest::Approx(1.0).epsilon(0.1));
    CHECK(check(d, "win_rate").status == DriftStatus::ok);
    CHECK(check(d, "rejections").status == DriftStatus::ok);
    CHECK(check(d, "kill_switch").status == DriftStatus::ok);
    const std::string text = format_drift(d);
    CHECK(text.find("Status: ok") != std::string::npos);
    CHECK(drift_to_json(d).find("\"status\": \"ok\"") != std::string::npos);
}

TEST_CASE("detect_drift: losses, fees, overtrading and operations are graded") {
    auto ref = analytics::analyze(synthetic("ref", 24 * 60, 0.0002, 0.004, 12, 0.55, "3", 1));

    // Steady losses: the return z-score alarms and the drawdown ratio grows.
    auto losing = analytics::analyze(synthetic("losing", 24 * 10, -0.004, 0.004, 12, 0.55, "3", 3));
    auto d1 = detect_drift(ref, losing);
    CHECK(check(d1, "return").status == DriftStatus::alarm);
    CHECK(check(d1, "return").score < -3.0);
    CHECK(check(d1, "drawdown").score > 1.0);
    CHECK(d1.status == DriftStatus::alarm);
    CHECK(d1.checks.size() == 5);  // no ops given

    // Fees tripled: fee drag alarms.
    auto costly = analytics::analyze(synthetic("costly", 24 * 10, 0.0002, 0.004, 12, 0.55, "9", 4));
    auto d2 = detect_drift(ref, costly);
    CHECK(check(d2, "fee_drag").score == doctest::Approx(3.0).epsilon(0.05));
    CHECK(check(d2, "fee_drag").status == DriftStatus::alarm);

    // Trading five times as often: trade rate alarms; half as often: warn.
    auto churn = analytics::analyze(synthetic("churn", 24 * 10, 0.0002, 0.004, 2, 0.55, "3", 5));
    CHECK(check(detect_drift(ref, churn), "trade_rate").status == DriftStatus::alarm);
    auto quiet = analytics::analyze(synthetic("quiet", 24 * 10, 0.0002, 0.004, 36, 0.55, "3", 6));  // ~1/3 the rate
    CHECK(check(detect_drift(ref, quiet), "trade_rate").status == DriftStatus::warn);

    // Win rate collapse over enough trips.
    auto unlucky = analytics::analyze(synthetic("unlucky", 24 * 30, 0.0002, 0.004, 6, 0.15, "3", 7));
    CHECK(check(detect_drift(ref, unlucky), "win_rate").status == DriftStatus::alarm);

    // Operations: rejections and a kill switch trip.
    auto d3 = detect_drift(ref, losing, ObservedOps{.orders = 20, .rejected = 5, .kill_switch_trips = 1});
    CHECK(check(d3, "rejections").status == DriftStatus::alarm);
    CHECK(check(d3, "kill_switch").status == DriftStatus::alarm);
    auto d4 = detect_drift(ref, losing, ObservedOps{.orders = 20, .rejected = 2, .kill_switch_trips = 0});
    CHECK(check(d4, "rejections").status == DriftStatus::warn);

    // Too little data: the statistical checks stay ok with a note.
    auto tiny = analytics::analyze(synthetic("tiny", 6, -0.01, 0.004, 12, 0.55, "3", 8));
    auto d5 = detect_drift(ref, tiny);
    CHECK(check(d5, "return").status == DriftStatus::ok);
    CHECK(check(d5, "return").detail == "insufficient data");
    CHECK(check(d5, "trade_rate").detail == "insufficient data");
}

TEST_CASE("detect_drift_dirs reads run directories; revalidate decides keep/watch/retire") {
    TempDir tmp;
    auto ref_result = synthetic("ref", 24 * 60, 0.0002, 0.004, 12, 0.55, "3", 1);
    auto obs_result = synthetic("obs", 24 * 10, 0.0002, 0.004, 12, 0.55, "3", 2);
    obs_result.summary.orders = 40;
    obs_result.summary.rejected = 0;
    obs_result.summary.kill_switch_trips = 0;
    REQUIRE(backtest::write_artifacts(ref_result, tmp.path / "ref").has_value());
    REQUIRE(backtest::write_artifacts(obs_result, tmp.path / "obs").has_value());
    auto d = detect_drift_dirs(tmp.path / "ref", tmp.path / "obs");
    REQUIRE_MESSAGE(d.has_value(), d.error().message);
    CHECK_MESSAGE(d->status == DriftStatus::ok, format_drift(*d));
    CHECK(d->checks.size() == 7);  // ops read from summary.json
    REQUIRE(write_drift(*d, tmp.path / "obs").has_value());
    CHECK(fs::exists(tmp.path / "obs" / "drift.txt"));
    CHECK(fs::exists(tmp.path / "obs" / "drift.json"));
    CHECK_FALSE(detect_drift_dirs(tmp.path / "nope", tmp.path / "obs").has_value());

    auto obs = analytics::analyze(obs_result);
    KillCriteria criteria;
    criteria.min_round_trips = 10;
    criteria.must_beat_benchmark = false;
    criteria.min_sharpe = -100.0;
    criteria.min_profit_factor = 0.0;
    criteria.max_drawdown = 1.0;
    auto keep = revalidate(*d, obs, criteria);
    CHECK(keep.decision == Decision::keep);
    CHECK(keep.reasons.empty());
    CHECK(keep.kill.pass);

    // A warning somewhere -> watch.
    DriftReport warned = *d;
    warned.checks[0].status = DriftStatus::warn;
    warned.status = DriftStatus::warn;
    auto watch = revalidate(warned, obs, criteria);
    CHECK(watch.decision == Decision::watch);
    CHECK(watch.reasons.size() == 1);

    // An alarm, a failed kill criterion or an inconsistent backtest -> retire.
    DriftReport alarmed = *d;
    alarmed.checks[1].status = DriftStatus::alarm;
    CHECK(revalidate(alarmed, obs, criteria).decision == Decision::retire);
    KillCriteria strict = criteria;
    strict.min_sharpe = 1000.0;
    auto killed = revalidate(*d, obs, strict);
    CHECK(killed.decision == Decision::retire);
    CHECK_FALSE(killed.kill.pass);
    ConsistencyResult inconsistent;
    inconsistent.consistent = false;
    inconsistent.return_gap = -0.05;
    auto rv = revalidate(*d, obs, criteria, inconsistent);
    CHECK(rv.decision == Decision::retire);
    CHECK(rv.reasons[0].find("differs by -5.00%") != std::string::npos);
    // Kill criteria are not applied before min_round_trips.
    KillCriteria patient = strict;
    patient.min_round_trips = 1000;
    CHECK(revalidate(*d, obs, patient).decision == Decision::keep);

    const std::string text = format_revalidation(rv);
    CHECK(text.find("Decision: retire") != std::string::npos);
    REQUIRE(write_revalidation(rv, tmp.path / "obs").has_value());
    CHECK(fs::exists(tmp.path / "obs" / "revalidation.txt"));
}
