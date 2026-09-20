#pragma once

// The strategy framework: the abstraction that makes a strategy portable
// across backtest, paper and live.
//
// A Strategy receives events and talks to the outside world only through
// its StrategyContext: market state, its own positions and orders, order
// entry (already behind the risk gate), timers, logging, metrics and
// parameters. It never sees a socket, a clock implementation, a venue
// type or a file. Everything a strategy needs to be reproducible (time,
// randomness) comes from the context.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/core/scheduler.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/market_data/order_book.hpp"
#include "tradebot/replay/latency.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tradebot::strategy {

struct OrderIntent {
    Side side;
    Quantity quantity;
    OrderType type = OrderType::limit;
    Price price;  // limit orders
    TimeInForce time_in_force = TimeInForce::gtc;
};

class StrategyContext {
public:
    virtual ~StrategyContext() = default;

    // Identity and configuration
    [[nodiscard]] virtual StrategyId id() const noexcept = 0;
    [[nodiscard]] virtual const Config& params() const noexcept = 0;
    [[nodiscard]] virtual Logger& log() noexcept = 0;
    [[nodiscard]] virtual replay::Rng& rng() noexcept = 0;

    // Time
    [[nodiscard]] virtual Timestamp now() const noexcept = 0;
    virtual TimerId schedule_every(Duration interval, TimerCallback cb) = 0;
    virtual TimerId schedule_at(Timestamp at, TimerCallback cb) = 0;
    virtual void cancel_timer(TimerId id) = 0;

    // Market state (client-side view, i.e. after latency)
    [[nodiscard]] virtual const Instrument& instrument() const noexcept = 0;
    [[nodiscard]] virtual const market_data::OrderBook& book() const noexcept = 0;
    [[nodiscard]] virtual bool book_synced() const noexcept = 0;
    [[nodiscard]] virtual std::optional<Price> last_trade_price() const noexcept = 0;
    [[nodiscard]] virtual std::optional<Price> mark() const noexcept = 0;
    // Ask the runner to build candles of this interval from trades and
    // deliver them via on_candle (in addition to any stored candles).
    virtual void request_candles(Duration interval) = 0;

    // Own state
    [[nodiscard]] virtual Quantity position() const = 0;
    [[nodiscard]] virtual Notional cash() const = 0;  // account cash
    [[nodiscard]] virtual Notional equity() const = 0;  // strategy equity contribution
    [[nodiscard]] virtual std::vector<execution::OrderState> open_orders() const = 0;

    // Orders (through the risk gate). Outcomes arrive on on_execution_report.
    [[nodiscard]] virtual Result<ClientOrderId> submit(const OrderIntent& intent) = 0;
    [[nodiscard]] virtual Result<void> cancel(ClientOrderId id) = 0;
    virtual void cancel_all() = 0;

    // Metrics for research (name, value at now()).
    virtual void metric(std::string_view name, double value) = 0;

    // Convenience wrappers
    [[nodiscard]] Result<ClientOrderId> buy_market(Quantity q) {
        return submit({Side::buy, q, OrderType::market, Price{}, TimeInForce::ioc});
    }
    [[nodiscard]] Result<ClientOrderId> sell_market(Quantity q) {
        return submit({Side::sell, q, OrderType::market, Price{}, TimeInForce::ioc});
    }
    [[nodiscard]] Result<ClientOrderId> buy_limit(Quantity q, Price p, TimeInForce tif = TimeInForce::gtc) {
        return submit({Side::buy, q, OrderType::limit, p, tif});
    }
    [[nodiscard]] Result<ClientOrderId> sell_limit(Quantity q, Price p, TimeInForce tif = TimeInForce::gtc) {
        return submit({Side::sell, q, OrderType::limit, p, tif});
    }
};

class Strategy {
public:
    virtual ~Strategy() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Lifecycle. The context outlives the strategy's active period; keep
    // the pointer from on_start.
    virtual void on_start(StrategyContext& ctx) { ctx_ = &ctx; }
    virtual void on_stop() {}

    // Events (client-side, after latency)
    virtual void on_trade(const market_data::Trade&) {}
    virtual void on_book_update() {}  // book() changed (snapshot or delta applied)
    virtual void on_book_ticker(const market_data::BookTicker&) {}
    virtual void on_candle(const market_data::Candle&) {}
    virtual void on_execution_report(const execution::ExecutionReport&) {}

protected:
    [[nodiscard]] StrategyContext& ctx() noexcept { return *ctx_; }
    [[nodiscard]] const StrategyContext& ctx() const noexcept { return *ctx_; }
    [[nodiscard]] bool started() const noexcept { return ctx_ != nullptr; }

private:
    StrategyContext* ctx_ = nullptr;
};

// Registry of strategies constructible by name (for configs and CLIs).
using StrategyFactory = std::function<std::unique_ptr<Strategy>()>;
class StrategyRegistry {
public:
    void add(std::string name, StrategyFactory factory) { factories_[std::move(name)] = std::move(factory); }
    [[nodiscard]] Result<std::unique_ptr<Strategy>> create(std::string_view name) const {
        auto it = factories_.find(std::string(name));
        if (it == factories_.end()) {
            return make_error(ErrorCode::not_found, "unknown strategy '" + std::string(name) + "'");
        }
        return it->second();
    }
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& [n, f] : factories_) out.push_back(n);
        return out;
    }

private:
    std::map<std::string, StrategyFactory> factories_;
};

}  // namespace tradebot::strategy
