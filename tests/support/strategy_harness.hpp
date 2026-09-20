#pragma once

// Runs one registered strategy through the full stack (replay engine,
// simulated exchange, portfolio, risk gate, runner) over a scripted event
// stream and returns what happened.

#include "tradebot/execution/simulated_exchange.hpp"
#include "tradebot/risk/risk_manager.hpp"
#include "tradebot/strategies/advanced.hpp"
#include "tradebot/strategies/baselines.hpp"
#include "tradebot/strategy/runner.hpp"

#include <doctest/doctest.h>

namespace tradebot::test {

inline const InstrumentId kHarnessEth{1};
inline const Timestamp kHarnessT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

inline Instrument harness_instrument() {
    using namespace tradebot::literals;
    return Instrument{.id = kHarnessEth, .venue = VenueId{1}, .symbol = "ETHUSDT", .base = "ETH",
                      .quote = "USDT", .tick_size = "0.01"_px, .lot_size = "0.0001"_qty,
                      .min_quantity = "0.0001"_qty, .min_notional = "5"_ntl};
}

// A deep symmetric book around `mid` at `at`.
inline market_data::BookSnapshot harness_book(Timestamp at, double mid, std::int64_t id, double bid_size = 1000,
                                              double ask_size = 1000) {
    using namespace tradebot::literals;
    market_data::BookSnapshot s;
    s.instrument = kHarnessEth;
    s.recv_time = at;
    s.last_update_id = id;
    for (int i = 0; i < 5; ++i) {
        s.bids.push_back({Price::from_double(mid - 0.5 - i).round_to("0.01"_px, RoundingMode::down), Quantity::from_double(bid_size)});
        s.asks.push_back({Price::from_double(mid + 0.5 + i).round_to("0.01"_px, RoundingMode::up), Quantity::from_double(ask_size)});
    }
    return s;
}

inline market_data::Trade harness_trade(Timestamp at, double price, std::uint64_t id, Side aggressor = Side::buy,
                                        double qty = 1.0) {
    using namespace tradebot::literals;
    market_data::Trade t;
    t.instrument = kHarnessEth;
    t.exchange_time = at;
    t.recv_time = at;
    t.id = TradeId{id};
    t.price = Price::from_double(price).round_to("0.01"_px, RoundingMode::nearest);
    t.quantity = Quantity::from_double(qty);
    t.aggressor = aggressor;
    return t;
}

// One-minute candles from a price path, book refreshed each minute.
inline std::vector<market_data::MarketEvent> harness_path_events(const std::vector<double>& path) {
    std::vector<market_data::MarketEvent> out;
    std::uint64_t id = 1;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Timestamp t = kHarnessT0 + Duration::minutes(static_cast<std::int64_t>(i)) + Duration::seconds(1);
        out.push_back(harness_book(t, path[i], static_cast<std::int64_t>(1000 + i)));
        out.push_back(harness_trade(t + Duration::seconds(1), path[i], id++));
    }
    const Timestamp end = kHarnessT0 + Duration::minutes(static_cast<std::int64_t>(path.size())) + Duration::seconds(1);
    out.push_back(harness_book(end, path.back(), 5000));
    return out;
}

struct HarnessResult {
    std::vector<execution::ExecutionReport> fills;
    std::vector<execution::ExecutionReport> reports;
    Quantity final_position;
    Notional final_equity;
    std::vector<strategy::MetricSample> metrics;
    execution::SimulatedExchange::Stats exchange;
};

inline HarnessResult run_strategy_events(const std::string& name, const std::string& params_text,
                                         std::vector<market_data::MarketEvent> events, std::uint64_t seed = 1,
                                         execution::SimulatedExchangeOptions ex_opts = {}) {
    using namespace tradebot::literals;
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    strategies::register_advanced(registry);
    auto strat = registry.create(name);
    REQUIRE_MESSAGE(strat.has_value(), strat.error().message);

    replay::VectorEventSource source(std::move(events));
    SimClock clock(kHarnessT0);
    replay::ZeroLatency latency;
    replay::ReplayEngine engine(source, clock, latency, {.seed = seed});
    execution::SimulatedExchange exchange(harness_instrument(), engine, latency, engine.rng(), ex_opts);
    engine.venue_bus().subscribe(exchange);
    portfolio::Portfolio pf("10000"_ntl);
    engine.bus().subscribe(pf);
    risk::RiskLimits limits;
    limits.max_price_deviation = 0;
    risk::RiskManager risk(exchange, pf, clock, limits);
    strategy::StrategyRunner runner(engine, risk, pf, harness_instrument(), Logger{}, seed);
    engine.bus().subscribe(runner);

    HarnessResult result;
    struct Tap final : execution::ExecutionListener {
        HarnessResult& r;
        execution::ExecutionListener& next;
        Tap(HarnessResult& res, execution::ExecutionListener& n) : r(res), next(n) {}
        void on_execution_report(const execution::ExecutionReport& rep) override {
            r.reports.push_back(rep);
            if (rep.type == execution::ReportType::fill) r.fills.push_back(rep);
            next.on_execution_report(rep);
        }
    } tap(result, runner);
    risk.set_listener(&tap);

    runner.add(std::move(*strat), *Config::parse(params_text));
    runner.start();
    REQUIRE(engine.run().has_value());
    runner.stop();
    result.final_position = pf.position(kHarnessEth);
    result.final_equity = pf.equity();
    result.metrics = runner.metrics();
    result.exchange = exchange.stats();
    return result;
}

inline HarnessResult run_strategy_path(const std::string& name, const std::string& params_text,
                                       const std::vector<double>& path, std::uint64_t seed = 1) {
    return run_strategy_events(name, params_text, harness_path_events(path), seed);
}

inline std::vector<double> harness_ramp(double start, double step, std::size_t n) {
    std::vector<double> p;
    for (std::size_t i = 0; i < n; ++i) p.push_back(start + step * static_cast<double>(i));
    return p;
}

inline std::vector<double> harness_concat(std::vector<double> a, const std::vector<double>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace tradebot::test
