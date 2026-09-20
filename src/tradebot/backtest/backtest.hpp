#pragma once

// Backtesting: one deterministic run of strategies over stored history.
//
// A BacktestSpec names everything that influences the result (data range,
// strategy and parameters, exchange model, latency, risk limits, capital,
// seed). run_backtest wires the store source, replay engine, simulated
// exchange, portfolio, risk gate and strategy runner, runs to the end, and
// returns a BacktestResult. write_artifacts saves a complete, auditable
// record of the run (spec echo, equity curve, fills, orders, metrics,
// summary) so any number can be traced back to its inputs.

#include "tradebot/core/config.hpp"
#include "tradebot/core/error.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/execution/simulated_exchange.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/portfolio/portfolio.hpp"
#include "tradebot/replay/latency.hpp"
#include "tradebot/risk/risk_manager.hpp"
#include "tradebot/storage/event_store.hpp"
#include "tradebot/strategy/runner.hpp"
#include "tradebot/strategy/strategy.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tradebot::backtest {

struct LatencySpec {
    enum class Kind { zero, constant, jitter } kind = Kind::jitter;
    Duration market_data = Duration::millis(5);
    Duration order = Duration::millis(5);
    Duration ack = Duration::millis(5);
    Duration jitter = Duration::millis(10);  // jitter kind only, applies to all three

    [[nodiscard]] std::unique_ptr<replay::LatencyModel> make() const;
};

struct StrategySpec {
    std::string name;  // registry name
    std::string label;  // human label; defaults to name
    Config params;
};

struct BacktestSpec {
    std::string run_id;  // derived from the spec when empty
    storage::StorePath store;
    Instrument instrument;
    Timestamp from;
    Timestamp to;
    std::vector<StrategySpec> strategies;
    Notional initial_cash;
    execution::SimulatedExchangeOptions exchange;
    LatencySpec latency;
    risk::RiskLimits limits;
    std::uint64_t seed = 1;
    Duration sample_interval = Duration::minutes(1);  // equity curve sampling
    bool load_tickers = false;
    std::vector<Duration> stored_candle_intervals;  // stored candles to feed (optional)

    // A stable textual form used for the run id and the config artifact.
    [[nodiscard]] std::string describe() const;
    [[nodiscard]] std::string derived_run_id() const;
};

// Parses a spec from a config with sections [backtest], [data], [instrument],
// [exchange], [risk], [strategy], [strategy.params]. See
// configs/backtest.example.conf.
[[nodiscard]] Result<BacktestSpec> parse_backtest_spec(const Config& cfg);

// Expands a [sweep] section (key = v1, v2, ...) into one spec per combination.
[[nodiscard]] Result<std::vector<BacktestSpec>> expand_sweep(const BacktestSpec& base,
                                                             const Config& sweep);

struct FillRecord {
    Timestamp time;
    StrategyId strategy;
    ClientOrderId client_id;
    Side side;
    Price price;
    Quantity quantity;
    Notional fee;
    Liquidity liquidity;
};

struct OrderRecord {
    Timestamp time;
    StrategyId strategy;
    ClientOrderId client_id;
    execution::ReportType type;
    Side side;
    OrderType order_type;
    Price price;
    Quantity filled;
    Quantity remaining;
    std::string reason;
};

struct BacktestSummary {
    Notional initial_cash;
    Notional final_equity;
    double total_return = 0.0;  // fraction
    Notional realized_pnl_net;
    Notional fees;
    Notional max_drawdown;
    double max_drawdown_fraction = 0.0;
    std::uint64_t fills = 0;
    std::uint64_t orders = 0;
    std::uint64_t rejected = 0;
    Quantity volume;
    Notional turnover;
    std::uint64_t events = 0;
    std::uint64_t kill_switch_trips = 0;
    Duration wall_time;
};

struct BacktestResult {
    BacktestSpec spec;
    std::vector<portfolio::EquitySample> equity_curve;
    std::vector<FillRecord> fills;
    std::vector<OrderRecord> orders;
    std::vector<strategy::MetricSample> metrics;
    std::vector<std::pair<StrategyId, std::string>> strategy_labels;
    BacktestSummary summary;
};

[[nodiscard]] Result<BacktestResult> run_backtest(const BacktestSpec& spec,
                                                  const strategy::StrategyRegistry& registry,
                                                  Logger log);

// Writes config.txt, equity.csv, fills.csv, orders.csv, metrics.csv and
// summary.json under <dir>. Creates the directory.
[[nodiscard]] Result<void> write_artifacts(const BacktestResult& result,
                                           const std::filesystem::path& dir);

// Runs several specs on `threads` workers; results keep the input order.
// Individual failures are returned in place, not aborting the batch.
[[nodiscard]] std::vector<Result<BacktestResult>> run_batch(
    const std::vector<BacktestSpec>& specs, const strategy::StrategyRegistry& registry,
    Logger log, unsigned threads);

}  // namespace tradebot::backtest
