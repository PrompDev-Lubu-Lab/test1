#pragma once

// StrategyRunner hosts strategies on one event stream and one venue.
//
// It subscribes to the client-side bus, keeps the client-side order book
// (through a BookSynchronizer, so a strategy's view of the book is exactly
// as good as its feed), builds requested candles from trades, dispatches
// events to every strategy, and routes execution reports first to the
// portfolio and then to the strategy that owns the order. Each strategy
// gets its own context, parameters, logger and seeded rng.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/core/scheduler.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/market_data/book_synchronizer.hpp"
#include "tradebot/market_data/candle_aggregator.hpp"
#include "tradebot/portfolio/portfolio.hpp"
#include "tradebot/replay/replay_engine.hpp"
#include "tradebot/strategy/strategy.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace tradebot::strategy {

struct MetricSample {
    Timestamp time;
    StrategyId strategy;
    std::string name;
    double value;
};

class StrategyRunner final : public replay::MarketDataListener, public execution::ExecutionListener {
public:
    StrategyRunner(Scheduler& scheduler, execution::ExecutionVenue& venue,
                   portfolio::Portfolio& portfolio, Instrument instrument, Logger log,
                   std::uint64_t seed = 1);
    ~StrategyRunner() override;

    // Adds a strategy with its own id and parameter section. Must be
    // called before start().
    StrategyId add(std::unique_ptr<Strategy> strategy, Config params, std::string label = {});

    void start();
    void stop();

    // --- MarketDataListener (client bus) ---------------------------------
    void on_trade(const market_data::Trade& t) override;
    void on_book_snapshot(const market_data::BookSnapshot& s) override;
    void on_book_delta(const market_data::BookDelta& d) override;
    void on_book_ticker(const market_data::BookTicker& bt) override;
    void on_candle(const market_data::Candle& c) override;

    // --- ExecutionListener (from the risk gate / venue) ------------------
    void on_execution_report(const execution::ExecutionReport& report) override;

    [[nodiscard]] const market_data::OrderBook& book() const noexcept { return sync_.book(); }
    [[nodiscard]] const std::vector<MetricSample>& metrics() const noexcept { return metrics_; }
    [[nodiscard]] std::vector<StrategyId> strategy_ids() const;
    [[nodiscard]] std::string label(StrategyId id) const;
    [[nodiscard]] Strategy* strategy(StrategyId id) const;

    struct Stats {
        std::uint64_t trades = 0;
        std::uint64_t book_updates = 0;
        std::uint64_t candles = 0;
        std::uint64_t reports = 0;
        std::uint64_t orders_submitted = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    class Context;
    struct Slot {
        StrategyId id;
        std::string label;
        std::unique_ptr<Strategy> strategy;
        std::unique_ptr<Context> context;
        std::vector<std::unique_ptr<market_data::CandleAggregator>> aggregators;
    };

    Scheduler& scheduler_;
    execution::ExecutionVenue& venue_;
    portfolio::Portfolio& portfolio_;
    Instrument instrument_;
    Logger log_;
    std::uint64_t seed_;
    market_data::BookSynchronizer sync_;
    std::optional<Price> last_trade_;
    std::vector<std::unique_ptr<Slot>> slots_;  // stable addresses for contexts
    std::unordered_map<ClientOrderId, StrategyId> order_owner_;
    IdGenerator<ClientOrderId> client_ids_;
    std::vector<MetricSample> metrics_;
    bool started_ = false;
    Stats stats_;
};

}  // namespace tradebot::strategy
