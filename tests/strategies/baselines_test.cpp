#include "tradebot/strategies/baselines.hpp"

#include "tradebot/execution/simulated_exchange.hpp"
#include "tradebot/risk/risk_manager.hpp"
#include "tradebot/strategy/runner.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::execution;
using namespace tradebot::strategies;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

Instrument eth() {
    return Instrument{.id = kEth, .venue = VenueId{1}, .symbol = "ETHUSDT", .base = "ETH",
                      .quote = "USDT", .tick_size = "0.01"_px, .lot_size = "0.0001"_qty,
                      .min_quantity = "0.0001"_qty, .min_notional = "5"_ntl};
}

// A deep book around `mid` so market orders always fill near the mark.
BookSnapshot book_at(Timestamp t, double mid, std::int64_t id) {
    BookSnapshot s;
    s.instrument = kEth;
    s.recv_time = t;
    s.last_update_id = id;
    s.bids = {{Price::from_double(mid - 0.5).round_to("0.01"_px, RoundingMode::down), "1000"_qty}};
    s.asks = {{Price::from_double(mid + 0.5).round_to("0.01"_px, RoundingMode::up), "1000"_qty}};
    return s;
}

// One trade per minute following `path`; a fresh book snapshot each minute
// keeps the venue's mark in step with the price.
std::vector<MarketEvent> path_events(const std::vector<double>& path) {
    std::vector<MarketEvent> out;
    std::uint64_t id = 1;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Timestamp t = kT0 + Duration::minutes(static_cast<std::int64_t>(i)) + Duration::seconds(1);
        out.push_back(book_at(t, path[i], static_cast<std::int64_t>(1000 + i)));
        Trade tr;
        tr.instrument = kEth;
        tr.exchange_time = t + Duration::seconds(1);
        tr.recv_time = tr.exchange_time;
        tr.id = TradeId{id++};
        tr.price = Price::from_double(path[i]).round_to("0.01"_px, RoundingMode::nearest);
        tr.quantity = "1"_qty;
        tr.aggressor = Side::buy;
        out.push_back(tr);
    }
    // One more minute so the last candle closes.
    const Timestamp end = kT0 + Duration::minutes(static_cast<std::int64_t>(path.size())) + Duration::seconds(1);
    out.push_back(book_at(end, path.back(), 5000));
    return out;
}

struct RunResult {
    std::vector<ExecutionReport> fills;
    Quantity final_position;
    Notional final_equity;
    std::vector<strategy::MetricSample> metrics;
};

RunResult run_strategy(const std::string& name, const std::string& params_text,
                       const std::vector<double>& path, std::uint64_t seed = 1) {
    strategy::StrategyRegistry registry;
    register_baselines(registry);
    auto strat = registry.create(name);
    REQUIRE_MESSAGE(strat.has_value(), strat.error().message);

    auto events = path_events(path);
    replay::VectorEventSource source(events);
    SimClock clock(kT0);
    replay::ZeroLatency latency;
    replay::ReplayEngine engine(source, clock, latency, {.seed = seed});
    SimulatedExchange exchange(eth(), engine, latency, engine.rng());
    engine.venue_bus().subscribe(exchange);
    portfolio::Portfolio pf("10000"_ntl);
    engine.bus().subscribe(pf);
    risk::RiskLimits limits;
    limits.max_price_deviation = 0;
    risk::RiskManager risk(exchange, pf, clock, limits);
    strategy::StrategyRunner runner(engine, risk, pf, eth(), Logger{}, seed);
    engine.bus().subscribe(runner);

    struct Capture final : replay::MarketDataListener {};
    std::vector<ExecutionReport> fills;
    struct Tap final : ExecutionListener {
        std::vector<ExecutionReport>& fills;
        ExecutionListener& next;
        Tap(std::vector<ExecutionReport>& f, ExecutionListener& n) : fills(f), next(n) {}
        void on_execution_report(const ExecutionReport& r) override {
            if (r.type == ReportType::fill) fills.push_back(r);
            next.on_execution_report(r);
        }
    } tap(fills, runner);
    risk.set_listener(&tap);

    runner.add(std::move(*strat), *Config::parse(params_text));
    runner.start();
    REQUIRE(engine.run().has_value());
    runner.stop();
    return RunResult{fills, pf.position(kEth), pf.equity(), runner.metrics()};
}

std::vector<double> ramp(double start, double step, std::size_t n) {
    std::vector<double> p;
    for (std::size_t i = 0; i < n; ++i) p.push_back(start + step * static_cast<double>(i));
    return p;
}

std::vector<double> concat(std::vector<double> a, const std::vector<double>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace

TEST_CASE("registry lists the baselines") {
    strategy::StrategyRegistry registry;
    register_baselines(registry);
    CHECK(registry.names() == std::vector<std::string>{"breakout", "buy_and_hold", "ma_crossover", "mean_reversion", "random"});
    CHECK_FALSE(registry.create("nope").has_value());
}

TEST_CASE("buy_and_hold: buys once with the equity fraction, then holds") {
    auto r = run_strategy("buy_and_hold", "equity_fraction = 0.5", ramp(3000, 1, 10));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].side == Side::buy);
    // 50% of 10000 at ~3000.5 ask => ~1.666 ETH, rounded down to lot size.
    CHECK(r.final_position > "1.66"_qty);
    CHECK(r.final_position < "1.67"_qty);
    CHECK(r.final_position == r.fills[0].fill->quantity);
    // Price rose ~9 over the run: equity above start minus fees.
    CHECK(r.final_equity > "10000"_ntl);

    auto fixed = run_strategy("buy_and_hold", "quantity = 0.25", ramp(3000, 1, 5));
    REQUIRE(fixed.fills.size() == 1);
    CHECK(fixed.final_position == "0.25"_qty);
}

TEST_CASE("ma_crossover: enters on golden cross, exits on death cross") {
    // Flat, then up (fast crosses above slow), then down (crosses below).
    auto path = concat(concat(ramp(3000, 0, 10), ramp(3000, 5, 15)), ramp(3070, -8, 15));
    auto r = run_strategy("ma_crossover", "fast = 3\nslow = 8\nquantity = 0.1", path);
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[1].side == Side::sell);
    CHECK(r.fills[0].time < r.fills[1].time);
    CHECK(r.final_position.is_zero());
    bool has_metric = false;
    for (const auto& m : r.metrics) has_metric |= m.name == "fast_minus_slow";
    CHECK(has_metric);
}

TEST_CASE("mean_reversion: buys the dip, sells the recovery") {
    // Flat 3000 for a window, then a sharp drop, then recovery to the mean.
    auto path = concat(concat(ramp(3000, 0, 12), {2990, 2980, 2970}), ramp(2975, 5, 12));
    auto r = run_strategy("mean_reversion", "lookback = 10\nentry_z = -1.5\nexit_z = 0\nquantity = 0.1", path);
    REQUIRE(r.fills.size() >= 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[0].fill->price < "2985"_px);  // bought during the drop
    CHECK(r.fills.back().side == Side::sell);
    CHECK(r.final_position.is_zero());
}

TEST_CASE("breakout: buys new highs, exits new lows") {
    auto path = concat(concat(ramp(3000, 0, 6), ramp(3001, 2, 6)), ramp(3011, -3, 8));
    auto r = run_strategy("breakout", "entry_lookback = 5\nexit_lookback = 3\nquantity = 0.1", path);
    REQUIRE(r.fills.size() == 2);
    CHECK(r.fills[0].side == Side::buy);
    CHECK(r.fills[0].fill->price > "3000"_px);
    CHECK(r.fills[1].side == Side::sell);
    CHECK(r.final_position.is_zero());
}

TEST_CASE("random: never trades at probability 0, is deterministic per seed") {
    auto none = run_strategy("random", "flip_probability = 0\nquantity = 0.1", ramp(3000, 1, 30));
    CHECK(none.fills.empty());
    auto a = run_strategy("random", "flip_probability = 0.5\nquantity = 0.1", ramp(3000, 1, 30), 11);
    auto b = run_strategy("random", "flip_probability = 0.5\nquantity = 0.1", ramp(3000, 1, 30), 11);
    auto c = run_strategy("random", "flip_probability = 0.5\nquantity = 0.1", ramp(3000, 1, 30), 12);
    CHECK_FALSE(a.fills.empty());
    REQUIRE(a.fills.size() == b.fills.size());
    for (std::size_t i = 0; i < a.fills.size(); ++i) {
        CHECK(a.fills[i].time == b.fills[i].time);
    }
    bool differs = a.fills.size() != c.fills.size();
    for (std::size_t i = 0; !differs && i < a.fills.size(); ++i) {
        differs = a.fills[i].time != c.fills[i].time;
    }
    CHECK(differs);
}
